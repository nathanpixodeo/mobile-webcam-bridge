#include "platform/Security.h"

#include <sddl.h>

#include <vector>

#include <wil/result.h>

namespace mwb::native {

std::wstring CurrentUserSid() {
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));

    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    THROW_LAST_ERROR_IF(size == 0);
    std::vector<BYTE> buffer(size);
    THROW_IF_WIN32_BOOL_FALSE(::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size));
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());

    wil::unique_hlocal_string sid;
    THROW_IF_WIN32_BOOL_FALSE(::ConvertSidToStringSidW(user->User.Sid, &sid));
    return std::wstring(sid.get());
}

SecurityAttributes::SecurityAttributes(std::wstring_view sddl) {
    const std::wstring text(sddl);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
                                                                                     &descriptor, nullptr));
    descriptor_.reset(descriptor);
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor;
    attributes_.bInheritHandle = FALSE;
}

std::wstring PrivatePipeSddl(std::wstring_view userSid) {
    std::wstring sddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;";
    sddl.append(userSid);
    sddl.append(L")");
    return sddl;
}

// LocalService (Frame Server) reads frames and writes its subscription: FILE_GENERIC_READ |
// FILE_WRITE_DATA. Never FILE_APPEND_DATA, which on a pipe means FILE_CREATE_PIPE_INSTANCE.
std::wstring PublicFramePipeSddl(std::wstring_view userSid) { return PrivatePipeSddl(userSid) + L"(A;;0x12008b;;;LS)"; }

}  // namespace mwb::native
