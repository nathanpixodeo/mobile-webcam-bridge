// Security descriptor helpers for the named pipes bridge-native creates.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include <wil/resource.h>

namespace mwb::native {

// String SID ("S-1-5-21-...") of the user the process runs as.
[[nodiscard]] std::wstring CurrentUserSid();

// SECURITY_ATTRIBUTES built from an SDDL string; keeps the descriptor alive.
class SecurityAttributes {
public:
    explicit SecurityAttributes(std::wstring_view sddl);
    SecurityAttributes(const SecurityAttributes&) = delete;
    SecurityAttributes& operator=(const SecurityAttributes&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* Get() noexcept { return &attributes_; }

private:
    wil::unique_hlocal descriptor_;
    SECURITY_ATTRIBUTES attributes_{};
};

// SDDL for pipes only the given user (plus SYSTEM and Administrators) may open; network logons
// are denied explicitly.
[[nodiscard]] std::wstring PrivatePipeSddl(std::wstring_view userSid);

// SDDL of the public frame pipe (protocol/FRAME_PIPE.md §2): as PrivatePipeSddl, plus read-only
// access for LocalService, the account of the Windows Camera Frame Server.
[[nodiscard]] std::wstring PublicFramePipeSddl(std::wstring_view userSid);

}  // namespace mwb::native
