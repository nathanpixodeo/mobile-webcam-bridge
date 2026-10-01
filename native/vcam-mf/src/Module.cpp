// DLL entry points of vcam-mf.dll: COM class factory, lifetime, and HKLM COM registration.
//
// Registration only writes the in-process server keys. Creating the virtual camera itself
// (MFCreateVirtualCamera) is `bridge-native install`'s job (protocol/BRIDGE_NATIVE.md).
#include "Framework.h"

#include "Activator.h"
#include "ModuleLifetime.h"

#include <mwb/Identifiers.h>
#include <mwb/um/Guid.h>
#include <mwb/um/Tracing.h>

#include <olectl.h>

#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;  // this module, without storing an HMODULE in DllMain

namespace {

constexpr GUID kSourceClsid = mwb::um::GuidFromString(mwb::ids::kMfSourceClsid);

class ClassFactory final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory> {
public:
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        RETURN_HR_IF_NULL(E_POINTER, object);
        *object = nullptr;
        RETURN_HR_IF(CLASS_E_NOAGGREGATION, outer != nullptr);

        wil::com_ptr_nothrow<mwb::vcam::Activator> activator;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<mwb::vcam::Activator>(activator.put()));
        return activator->QueryInterface(riid, object);
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            mwb::vcam::ModuleLifetime::Lock();
        } else {
            mwb::vcam::ModuleLifetime::Unlock();
        }
        return S_OK;
    }

private:
    mwb::vcam::ModuleObjectToken moduleToken_;
};

std::wstring ClsidKeyPath() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + mwb::ids::kMfSourceClsid;
}

HRESULT SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) noexcept {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RETURN_IF_WIN32_ERROR(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes));
    return S_OK;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID reserved) {
    // Keep this trivial: the DLL is loaded by Frame Server, Frame Server Monitor and client
    // processes, all under the loader lock here. No DisableThreadLibraryCalls: we use the static CRT.
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            mwb::um::trace::Register(L"vcam-mf");
            mwb::um::trace::InstallWilFailureLogging();
            break;
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) mwb::um::trace::Unregister();  // dynamic unload only
            break;
        default:
            break;
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* object) {
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    if (clsid != kSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;

    const Microsoft::WRL::ComPtr<ClassFactory> factory = Microsoft::WRL::Make<ClassFactory>();
    RETURN_IF_NULL_ALLOC(factory.Get());
    return factory->QueryInterface(riid, object);
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
    return mwb::vcam::ModuleLifetime::CanUnload() ? S_OK : S_FALSE;
}

// HKLM only: Frame Server runs as LocalService and never sees per-user (HKCU) registrations.
STDAPI DllRegisterServer() try {
    const std::wstring modulePath = wil::GetModuleFileNameW<std::wstring>(reinterpret_cast<HMODULE>(&__ImageBase));

    wil::unique_hkey classKey;
    RETURN_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                          KEY_WRITE, nullptr, classKey.put(), nullptr));
    RETURN_IF_FAILED(SetStringValue(classKey.get(), nullptr, L"MobileWebcamBridge Virtual Camera Source"));

    wil::unique_hkey serverKey;
    RETURN_IF_WIN32_ERROR(RegCreateKeyExW(classKey.get(), L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                          KEY_WRITE, nullptr, serverKey.put(), nullptr));
    RETURN_IF_FAILED(SetStringValue(serverKey.get(), nullptr, modulePath));
    RETURN_IF_FAILED(SetStringValue(serverKey.get(), L"ThreadingModel", L"Both"));
    return S_OK;
}
CATCH_RETURN()

STDAPI DllUnregisterServer() try {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str());
    if (status == ERROR_FILE_NOT_FOUND) return S_OK;
    RETURN_IF_WIN32_ERROR(status);
    return S_OK;
}
CATCH_RETURN()
