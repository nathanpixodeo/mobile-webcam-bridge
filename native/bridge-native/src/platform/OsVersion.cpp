#include "platform/OsVersion.h"

#include <windows.h>

#include <wil/result.h>

namespace mwb::native {

OsVersion QueryOsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    THROW_LAST_ERROR_IF_NULL(ntdll);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
    THROW_LAST_ERROR_IF_NULL(rtlGetVersion);

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    THROW_HR_IF(E_FAIL, rtlGetVersion(&info) != 0);
    return OsVersion{info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber};
}

}  // namespace mwb::native
