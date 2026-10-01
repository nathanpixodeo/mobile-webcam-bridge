#include "core/Strings.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

#include <wil/result.h>

namespace mwb::native {

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    THROW_LAST_ERROR_IF(length <= 0);
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                          nullptr);
    return result;
}

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(length <= 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
    return result;
}

namespace {

constexpr wchar_t LowerAscii(wchar_t c) noexcept { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; }

}  // namespace

bool EqualsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

bool ContainsIgnoreCaseAscii(std::wstring_view text, std::wstring_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        if (EqualsIgnoreCaseAscii(text.substr(start, needle.size()), needle)) return true;
    }
    return false;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool IsLowerHex(std::wstring_view text, std::size_t minLength, std::size_t maxLength) noexcept {
    if (text.size() < minLength || text.size() > maxLength) return false;
    for (const wchar_t c : text) {
        const bool digit = c >= L'0' && c <= L'9';
        const bool letter = c >= L'a' && c <= L'f';
        if (!digit && !letter) return false;
    }
    return true;
}

std::wstring RandomHexToken(std::size_t bytes) {
    std::vector<unsigned char> random(bytes);
    const NTSTATUS status = ::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) THROW_HR(HRESULT_FROM_NT(status));
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring token;
    token.reserve(bytes * 2);
    for (const unsigned char value : random) {
        token.push_back(kDigits[value >> 4]);
        token.push_back(kDigits[value & 0x0F]);
    }
    return token;
}

}  // namespace mwb::native
