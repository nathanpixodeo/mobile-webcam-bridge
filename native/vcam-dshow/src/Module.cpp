// DLL entry points of vcam-dshow.dll. DllGetClassObject/DllCanUnloadNow come from the BaseClasses
// (dllentry.cpp) and are driven by g_Templates below. Registration follows tshino/softcam (MIT):
// COM server keys via AMovieDllRegisterServer2, then the device entry in
// CLSID_VideoInputDeviceCategory through IFilterMapper2.
#include "Framework.h"

#include "VCamFilter.h"

#include <mwb/Identifiers.h>
#include <mwb/um/Guid.h>
#include <mwb/um/Tracing.h>

#include <olectl.h>

namespace {

constexpr GUID kFilterClsid = mwb::um::GuidFromString(mwb::ids::kDShowFilterClsid);

const AMOVIESETUP_MEDIATYPE kPinTypes[] = {
    {&MEDIATYPE_Video, &MEDIASUBTYPE_NULL},
};

const AMOVIESETUP_PIN kPins[] = {
    {
        const_cast<LPWSTR>(L"Capture"),  // name
        FALSE,                           // rendered
        TRUE,                            // output
        FALSE,                           // zero instances allowed
        FALSE,                           // many instances allowed
        &CLSID_NULL,                     // connects to filter
        nullptr,                         // connects to pin
        ARRAYSIZE(kPinTypes),
        kPinTypes,
    },
};

const REGFILTER2 kRegistration = {
    1,                 // version: REGFILTERPINS
    MERIT_DO_NOT_USE,  // never picked by intelligent connect; apps choose it explicitly
    ARRAYSIZE(kPins),
    kPins,
};

// Scoped COM initialisation for regsvr32's thread (which may already be initialised).
class ComInit final {
public:
    ComInit() noexcept : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    [[nodiscard]] HRESULT Result() const noexcept { return hr_ == RPC_E_CHANGED_MODE ? S_OK : hr_; }

private:
    HRESULT hr_;
};

HRESULT WithFilterMapper(HRESULT (*action)(IFilterMapper2*)) noexcept {
    const ComInit com;
    if (FAILED(com.Result())) return com.Result();

    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                  reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr)) {
        hr = action(mapper);
        mapper->Release();
    }
    CoFreeUnusedLibraries();
    return hr;
}

}  // namespace

// COM object table consumed by the BaseClasses class factory.
CFactoryTemplate g_Templates[] = {
    {
        mwb::ids::kDShowFriendlyName,
        &kFilterClsid,
        &mwb::dshow::VCamFilter::CreateInstance,
        nullptr,
        nullptr,  // registered in the video input category below, not the legacy filter category
    },
};
int g_cTemplates = ARRAYSIZE(g_Templates);

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            mwb::um::trace::Register(L"vcam-dshow");
            mwb::um::trace::InstallWilFailureLogging();
            break;
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) mwb::um::trace::Unregister();
            break;
        default:
            break;
    }
    return DllEntryPoint(module, reason, reserved);
}

STDAPI DllRegisterServer() {
    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr)) return hr;
    return WithFilterMapper([](IFilterMapper2* mapper) -> HRESULT {
        // Re-registration replaces any stale entry (e.g. from a previous install path).
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, kFilterClsid);
        return mapper->RegisterFilter(kFilterClsid, mwb::ids::kDShowFriendlyName, nullptr,
                                      &CLSID_VideoInputDeviceCategory, mwb::ids::kDShowFriendlyName, &kRegistration);
    });
}

STDAPI DllUnregisterServer() {
    const HRESULT unregisterDevice = WithFilterMapper([](IFilterMapper2* mapper) -> HRESULT {
        const HRESULT hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, mwb::ids::kDShowFriendlyName,
                                                    kFilterClsid);
        // Not registered is fine: uninstall must be idempotent.
        return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? S_OK : hr;
    });
    const HRESULT unregisterServer = AMovieDllRegisterServer2(FALSE);
    return FAILED(unregisterDevice) ? unregisterDevice : unregisterServer;
}
