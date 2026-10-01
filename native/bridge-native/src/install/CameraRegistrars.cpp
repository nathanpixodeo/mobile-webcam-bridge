#include "install/CameraRegistrars.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <string>
#include <vector>

#include <mwb/Identifiers.h>
#include <wil/com.h>
#include <wil/resource.h>

#include "core/Errors.h"
#include "core/Strings.h"
#include "platform/Paths.h"
#include "platform/Process.h"
#include "platform/Registry.h"

namespace mwb::native {

namespace {

void CallRegistrationExport(const std::filesystem::path& dll, const char* exportName) {
    const std::string name = ToUtf8(dll.filename().native());
    const wil::unique_hmodule module(::LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!module) throw ErrorFromWin32(::GetLastError(), "Cannot load " + name);

    using RegistrationFn = HRESULT(STDAPICALLTYPE*)();
    const auto function = reinterpret_cast<RegistrationFn>(::GetProcAddress(module.get(), exportName));
    if (function == nullptr) throw CommandError(ErrorCode::Internal, name + " does not export " + exportName);

    const HRESULT hr = function();
    if (FAILED(hr)) throw ErrorFromHresult(hr, std::string(exportName) + " of " + name);
}

void RunRegsvr32(const std::filesystem::path& regsvr32, const std::filesystem::path& dll, bool unregister) {
    std::vector<std::wstring> arguments{L"/s"};
    if (unregister) arguments.emplace_back(L"/u");
    arguments.push_back(dll.native());

    const unsigned long exitCode = RunAndWait(regsvr32, arguments, 60'000);
    if (exitCode == 0) return;

    const std::string name = ToUtf8(dll.filename().native());
    std::string reason;
    switch (exitCode) {
        case 3: reason = "could not load " + name; break;
        case 4: reason = name + " lacks the registration entry point"; break;
        case 5: reason = "the registration function of " + name + " failed"; break;
        default: reason = "exit code " + std::to_string(exitCode); break;
    }
    throw CommandError(ErrorCode::Internal, std::string(unregister ? "Unregistering" : "Registering") + " the DirectShow filter failed: " + reason);
}

std::filesystem::path Regsvr32In(const std::filesystem::path& systemDirectory) { return systemDirectory / L"regsvr32.exe"; }

}  // namespace

void ComServerModule::Register(const std::filesystem::path& dll) { CallRegistrationExport(dll, "DllRegisterServer"); }

void ComServerModule::Unregister(const std::filesystem::path& dll) { CallRegistrationExport(dll, "DllUnregisterServer"); }

// ---------------------------------------------------------------------------------------------

struct MfVirtualCameraRegistrar::Api {
    using CreateFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR,
                                              LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
    using StartupFn = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
    using ShutdownFn = HRESULT(STDAPICALLTYPE*)();

    wil::unique_hmodule sensorGroup;
    wil::unique_hmodule mediaPlatform;
    CreateFn create = nullptr;
    ShutdownFn shutdown = nullptr;
    bool started = false;

    [[nodiscard]] wil::com_ptr<IMFVirtualCamera> Open(std::wstring_view friendlyName) const {
        const std::wstring name(friendlyName);
        wil::com_ptr<IMFVirtualCamera> camera;
        const HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                  MFVirtualCameraAccess_AllUsers, name.c_str(), mwb::ids::kMfSourceClsid, nullptr, 0,
                                  camera.put());
        if (hr == E_ACCESSDENIED) {
            throw CommandError(ErrorCode::AccessDenied,
                               "MFCreateVirtualCamera was denied: camera access is blocked in Windows privacy settings, "
                               "or the process is not elevated");
        }
        if (FAILED(hr)) throw ErrorFromHresult(hr, "MFCreateVirtualCamera");
        return camera;
    }
};

MfVirtualCameraRegistrar::MfVirtualCameraRegistrar() : api_(std::make_unique<Api>()) {
    api_->sensorGroup.reset(::LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (api_->sensorGroup) {
        api_->create = reinterpret_cast<Api::CreateFn>(::GetProcAddress(api_->sensorGroup.get(), "MFCreateVirtualCamera"));
    }
    if (api_->create == nullptr) {
        throw CommandError(ErrorCode::NotSupportedOs,
                           "MFCreateVirtualCamera is not available; the Media Foundation camera needs Windows 11");
    }

    api_->mediaPlatform.reset(::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (api_->mediaPlatform) {
        const auto startup = reinterpret_cast<Api::StartupFn>(::GetProcAddress(api_->mediaPlatform.get(), "MFStartup"));
        api_->shutdown = reinterpret_cast<Api::ShutdownFn>(::GetProcAddress(api_->mediaPlatform.get(), "MFShutdown"));
        api_->started = startup != nullptr && api_->shutdown != nullptr && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_FULL));
    }
}

MfVirtualCameraRegistrar::~MfVirtualCameraRegistrar() {
    if (api_ && api_->started) api_->shutdown();
}

void MfVirtualCameraRegistrar::Create(std::wstring_view friendlyName) const {
    const wil::com_ptr<IMFVirtualCamera> camera = api_->Open(friendlyName);
    const HRESULT hr = camera->Start(nullptr);
    if (FAILED(hr)) throw ErrorFromHresult(hr, "Starting the virtual camera");
    // Released without Shutdown(): a system-lifetime camera stays registered.
}

void MfVirtualCameraRegistrar::Remove(std::wstring_view friendlyName) const {
    const wil::com_ptr<IMFVirtualCamera> camera = api_->Open(friendlyName);
    const HRESULT hr = camera->Remove();
    if (FAILED(hr)) throw ErrorFromHresult(hr, "Removing the virtual camera");
}

// ---------------------------------------------------------------------------------------------

DShowFilterRegistrar::DShowFilterRegistrar(std::filesystem::path x64Dll, std::optional<std::filesystem::path> x86Dll)
    : x64Dll_(std::move(x64Dll)), x86Dll_(std::move(x86Dll)) {}

void DShowFilterRegistrar::Register() const {
    RunRegsvr32(Regsvr32In(SystemDirectory()), x64Dll_, false);
    if (x86Dll_) {
        const std::optional<std::filesystem::path> wow64 = SystemWow64Directory();
        if (!wow64) throw CommandError(ErrorCode::NotSupportedOs, "WOW64 is not available; cannot register the x86 filter");
        RunRegsvr32(Regsvr32In(*wow64), *x86Dll_, false);
    }
}

void DShowFilterRegistrar::Unregister() const {
    if (x86Dll_) {
        if (const std::optional<std::filesystem::path> wow64 = SystemWow64Directory()) {
            RunRegsvr32(Regsvr32In(*wow64), *x86Dll_, true);
        }
    }
    RunRegsvr32(Regsvr32In(SystemDirectory()), x64Dll_, true);
}

void DeleteComClassKeys(std::wstring_view clsid) {
    // CLSID_VideoInputDeviceCategory: IFilterMapper2 registers capture filters under it.
    static constexpr wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
    const std::wstring classKey = L"SOFTWARE\\Classes\\CLSID\\" + std::wstring(clsid);
    const std::wstring categoryInstanceKey =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVideoInputCategory + L"\\Instance\\" + std::wstring(clsid);
    for (const RegistryView view : {RegistryView::Native64, RegistryView::Wow32}) {
        RegistryKey::DeleteTree(HKEY_LOCAL_MACHINE, classKey, view);
        RegistryKey::DeleteTree(HKEY_LOCAL_MACHINE, categoryInstanceKey, view);
    }
}

}  // namespace mwb::native
