#include "install/MicDriver.h"

#include <windows.h>
#include <setupapi.h>  // before newdev.h, which uses its types
#include <cfgmgr32.h>
#include <newdev.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include <mwb/MwbMicIoctl.h>
#include <wil/resource.h>

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

namespace {

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO handle) noexcept : handle_(handle) {}
    ~DeviceInfoSet() {
        if (IsValid()) ::SetupDiDestroyDeviceInfoList(handle_);
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HDEVINFO Get() const noexcept { return handle_; }

private:
    HDEVINFO handle_;
};

std::vector<std::wstring> ReadMultiString(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type = 0;
    DWORD required = 0;
    ::SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, nullptr, 0, &required);
    if (required == 0) return {};

    std::vector<wchar_t> buffer(required / sizeof(wchar_t) + 2, L'\0');
    if (!::SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, reinterpret_cast<BYTE*>(buffer.data()),
                                             static_cast<DWORD>((buffer.size() - 2) * sizeof(wchar_t)), nullptr) ||
        type != REG_MULTI_SZ) {
        return {};
    }
    std::vector<std::wstring> values;
    for (const wchar_t* item = buffer.data(); *item != L'\0'; item += std::wcslen(item) + 1) values.emplace_back(item);
    return values;
}

bool HasBridgeHardwareId(HDEVINFO set, SP_DEVINFO_DATA& device) {
    for (const std::wstring& id : ReadMultiString(set, device, SPDRP_HARDWAREID)) {
        if (EqualsIgnoreCaseAscii(id, MWBMIC_HARDWARE_ID_W)) return true;
    }
    return false;
}

// Name of the driver-store INF the node currently uses (e.g. "oem42.inf"), or empty.
std::wstring ReadDriverInfName(HDEVINFO set, SP_DEVINFO_DATA& device) {
    const HKEY raw = ::SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (raw == reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE)) return {};
    const wil::unique_hkey key(raw);

    wchar_t value[MAX_PATH] = {};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    if (::RegQueryValueExW(key.get(), L"InfPath", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ) {
        return {};
    }
    return std::wstring(value);
}

// Every device node (present or not) carrying our hardware id; the set stays open for the
// duration of `visit`, so the callback may act on the node.
template <typename Visit>
void ForEachBridgeNode(Visit&& visit) {
    const DeviceInfoSet set(::SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES));
    if (!set.IsValid()) throw ErrorFromWin32(::GetLastError(), "Cannot enumerate devices");

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!::SetupDiEnumDeviceInfo(set.Get(), index, &device)) {
            if (::GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw ErrorFromWin32(::GetLastError(), "Cannot enumerate devices");
        }
        if (HasBridgeHardwareId(set.Get(), device)) visit(set.Get(), device);
    }
}

void CreateDeviceNode(const GUID& classGuid, const wchar_t* className) {
    const DeviceInfoSet set(::SetupDiCreateDeviceInfoList(&classGuid, nullptr));
    if (!set.IsValid()) throw ErrorFromWin32(::GetLastError(), "Cannot create a device information set");

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!::SetupDiCreateDeviceInfoW(set.Get(), className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &device)) {
        throw ErrorFromWin32(::GetLastError(), "Cannot create the microphone device node");
    }
    static constexpr wchar_t kHardwareIds[] = MWBMIC_HARDWARE_ID_W L"\0";  // REG_MULTI_SZ
    if (!::SetupDiSetDeviceRegistryPropertyW(set.Get(), &device, SPDRP_HARDWAREID,
                                             reinterpret_cast<const BYTE*>(kHardwareIds), sizeof(kHardwareIds))) {
        throw ErrorFromWin32(::GetLastError(), "Cannot set the microphone hardware id");
    }
    if (!::SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.Get(), &device)) {
        throw ErrorFromWin32(::GetLastError(), "Cannot register the microphone device node");
    }
}

bool IsOemInfName(std::wstring_view name) noexcept {
    if (name.size() < 8) return false;  // "oemN.inf"
    return EqualsIgnoreCaseAscii(name.substr(0, 3), L"oem") &&
           EqualsIgnoreCaseAscii(name.substr(name.size() - 4), L".inf");
}

void RemoveDriverPackage(const std::wstring& infName, bool& rebootRequired) {
    // Never touch inbox INFs, only third-party packages published as oemNN.inf.
    if (!IsOemInfName(infName)) return;

    wchar_t windowsDirectory[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) throw ErrorFromWin32(::GetLastError(), "Cannot locate the Windows directory");
    const std::wstring infPath = std::wstring(windowsDirectory) + L"\\INF\\" + infName;

    using DiUninstallDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
    const HMODULE newdev = ::GetModuleHandleW(L"newdev.dll");
    const auto uninstallDriver = newdev != nullptr
                                     ? reinterpret_cast<DiUninstallDriverFn>(::GetProcAddress(newdev, "DiUninstallDriverW"))
                                     : nullptr;
    if (uninstallDriver != nullptr) {
        BOOL reboot = FALSE;
        if (uninstallDriver(nullptr, infPath.c_str(), 0, &reboot)) {
            rebootRequired = rebootRequired || reboot != FALSE;
            return;
        }
    }
    // Windows 10 before 1903 has no DiUninstallDriverW.
    if (!::SetupUninstallOEMInfW(infName.c_str(), SUOI_FORCEDELETE, nullptr)) {
        throw ErrorFromWin32(::GetLastError(), "Cannot remove driver package " + ToUtf8(infName));
    }
}

}  // namespace

MicDeviceState MicDriver::QueryState() const {
    MicDeviceState state;
    ForEachBridgeNode([&](HDEVINFO, SP_DEVINFO_DATA& device) {
        state.installed = true;
        ULONG status = 0;
        ULONG problem = 0;
        if (::CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) == CR_SUCCESS) {
            state.present = true;
            state.problemCode = (status & DN_HAS_PROBLEM) != 0 ? problem : 0;
        }
    });
    return state;
}

DriverChange MicDriver::Install(const std::filesystem::path& infPath) const {
    GUID classGuid{};
    wchar_t className[MAX_CLASS_NAME_LEN] = {};
    if (!::SetupDiGetINFClassW(infPath.c_str(), &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) {
        throw ErrorFromWin32(::GetLastError(), "Cannot read the class of " + ToUtf8(infPath.filename().native()));
    }

    DriverChange change;
    if (!QueryState().installed) {
        CreateDeviceNode(classGuid, className);
        change.createdDeviceNode = true;
    }

    BOOL reboot = FALSE;
    if (!::UpdateDriverForPlugAndPlayDevicesW(nullptr, MWBMIC_HARDWARE_ID_W, infPath.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD error = ::GetLastError();
        if (change.createdDeviceNode) {
            try {
                (void)Uninstall();
            } catch (...) {
                // Best effort: the original error is the one worth reporting.
            }
        }
        throw ErrorFromWin32(error, "Installing the virtual microphone driver failed");
    }
    change.rebootRequired = reboot != FALSE;
    return change;
}

DriverChange MicDriver::Uninstall() const {
    DriverChange change;
    std::vector<std::wstring> infNames;
    ForEachBridgeNode([&](HDEVINFO set, SP_DEVINFO_DATA& device) {
        const std::wstring infName = ReadDriverInfName(set, device);
        if (!infName.empty() && std::find(infNames.begin(), infNames.end(), infName) == infNames.end()) {
            infNames.push_back(infName);
        }
        BOOL reboot = FALSE;
        if (!::DiUninstallDevice(nullptr, set, &device, 0, &reboot)) {
            throw ErrorFromWin32(::GetLastError(), "Cannot remove the microphone device node");
        }
        change.rebootRequired = change.rebootRequired || reboot != FALSE;
    });
    for (const std::wstring& infName : infNames) RemoveDriverPackage(infName, change.rebootRequired);
    return change;
}

}  // namespace mwb::native
