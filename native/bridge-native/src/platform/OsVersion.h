// True OS version (RtlGetVersion is not subject to the GetVersionEx compatibility shims).
#pragma once

namespace mwb::native {

struct OsVersion {
    unsigned long major = 0;
    unsigned long minor = 0;
    unsigned long build = 0;

    // MFCreateVirtualCamera exists from Windows 11 (build 22000).
    [[nodiscard]] bool IsWindows11OrLater() const noexcept { return build >= 22000; }
};

[[nodiscard]] OsVersion QueryOsVersion();

}  // namespace mwb::native
