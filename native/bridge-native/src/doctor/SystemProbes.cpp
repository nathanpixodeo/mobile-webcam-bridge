#include "doctor/SystemProbes.h"

#include <windows.h>
#include <objbase.h>
#include <winsvc.h>
#include <initguid.h>  // defines the property keys used below in this translation unit
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

#include "core/Strings.h"
#include "platform/Com.h"
#include "platform/Registry.h"

namespace mwb::native {

namespace {

using ServiceHandle = wil::unique_any<SC_HANDLE, decltype(&::CloseServiceHandle), ::CloseServiceHandle>;

constexpr wchar_t kConsentStore[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";

ConsentValue ReadConsent(HKEY root, const std::wstring& path) {
    const std::optional<RegistryKey> key = RegistryKey::Open(root, path);
    if (!key) return ConsentValue::Unknown;
    const std::optional<std::wstring> value = key->GetString(L"Value");
    if (!value) return ConsentValue::Unknown;
    if (EqualsIgnoreCaseAscii(*value, L"Allow")) return ConsentValue::Allow;
    if (EqualsIgnoreCaseAscii(*value, L"Deny")) return ConsentValue::Deny;
    return ConsentValue::Unknown;
}

class PropVariant {
public:
    PropVariant() noexcept { ::PropVariantInit(&value_); }
    ~PropVariant() { ::PropVariantClear(&value_); }
    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;

    [[nodiscard]] PROPVARIANT* Get() noexcept { return &value_; }
    [[nodiscard]] std::wstring String() const {
        return value_.vt == VT_LPWSTR && value_.pwszVal != nullptr ? std::wstring(value_.pwszVal) : std::wstring();
    }

private:
    PROPVARIANT value_{};
};

std::wstring ReadStringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    PropVariant value;
    if (FAILED(store->GetValue(key, value.Get()))) return {};
    return value.String();
}

}  // namespace

ServiceInfo QueryService(std::wstring_view name) {
    ServiceInfo info;
    const ServiceHandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) return info;
    const std::wstring serviceName(name);
    const ServiceHandle service(::OpenServiceW(manager.get(), serviceName.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service) return info;

    info.startType = ServiceStartType::Other;
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    if (needed > 0) {
        std::vector<BYTE> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (::QueryServiceConfigW(service.get(), config, needed, &needed)) {
            switch (config->dwStartType) {
                case SERVICE_DISABLED: info.startType = ServiceStartType::Disabled; break;
                case SERVICE_DEMAND_START: info.startType = ServiceStartType::Manual; break;
                case SERVICE_AUTO_START:
                case SERVICE_BOOT_START:
                case SERVICE_SYSTEM_START: info.startType = ServiceStartType::Automatic; break;
                default: break;
            }
        }
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) {
        info.running = status.dwCurrentState == SERVICE_RUNNING;
    }
    return info;
}

PrivacyConsent QueryPrivacyConsent(std::wstring_view capability) {
    const std::wstring path = std::wstring(kConsentStore) + std::wstring(capability);
    PrivacyConsent consent;
    consent.machine = ReadConsent(HKEY_LOCAL_MACHINE, path);
    consent.user = ReadConsent(HKEY_CURRENT_USER, path);
    consent.desktopApps = ReadConsent(HKEY_CURRENT_USER, path + L"\\NonPackaged");
    return consent;
}

std::optional<bool> IsTestSigningEnabled() {
    struct SystemCodeIntegrityInformation {
        ULONG Length;
        ULONG CodeIntegrityOptions;
    };
    constexpr ULONG kSystemCodeIntegrityInformation = 103;
    constexpr ULONG kCodeIntegrityOptionTestSign = 0x02;
    using QueryFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return std::nullopt;
    const auto query = reinterpret_cast<QueryFn>(::GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (query == nullptr) return std::nullopt;

    SystemCodeIntegrityInformation info{sizeof(SystemCodeIntegrityInformation), 0};
    if (query(kSystemCodeIntegrityInformation, &info, sizeof(info), nullptr) != 0) return std::nullopt;
    return (info.CodeIntegrityOptions & kCodeIntegrityOptionTestSign) != 0;
}

std::optional<bool> IsSecureBootEnabled() {
    const std::optional<RegistryKey> key =
        RegistryKey::Open(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State");
    if (!key) return std::nullopt;
    const std::optional<DWORD> value = key->GetDword(L"UEFISecureBootEnabled");
    if (!value) return std::nullopt;
    return *value != 0;
}

bool PipeExists(std::wstring_view name) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileW(L"\\\\.\\pipe\\*", &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    const auto close = wil::scope_exit([&] { ::FindClose(find); });
    do {
        if (EqualsIgnoreCaseAscii(data.cFileName, name)) return true;
    } while (::FindNextFileW(find, &data));
    return false;
}

std::vector<AudioEndpointInfo> FindBridgeCaptureEndpoints() {
    std::vector<AudioEndpointInfo> endpoints;
    const ComApartment apartment;
    if (!apartment.Usable()) return endpoints;

    wil::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(enumerator.put())))) {
        return endpoints;
    }
    wil::com_ptr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATEMASK_ALL, devices.put()))) return endpoints;
    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return endpoints;

    for (UINT i = 0; i < count; ++i) {
        wil::com_ptr<IMMDevice> device;
        wil::com_ptr<IPropertyStore> store;
        if (FAILED(devices->Item(i, device.put())) || FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) continue;
        const std::wstring name = ReadStringProperty(store.get(), PKEY_Device_FriendlyName);
        const std::wstring adapter = ReadStringProperty(store.get(), PKEY_DeviceInterface_FriendlyName);
        const bool ours = ContainsIgnoreCaseAscii(name, L"Mobile Webcam Microphone") || ContainsIgnoreCaseAscii(name, L"MobileWebcamBridge") ||
                          ContainsIgnoreCaseAscii(adapter, L"Mobile Webcam Microphone") || ContainsIgnoreCaseAscii(adapter, L"MobileWebcamBridge");
        if (!ours) continue;
        DWORD state = 0;
        device->GetState(&state);
        endpoints.push_back(AudioEndpointInfo{name.empty() ? adapter : name, state == DEVICE_STATE_ACTIVE});
    }
    return endpoints;
}

}  // namespace mwb::native
