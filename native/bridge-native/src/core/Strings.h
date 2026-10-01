// String helpers: UTF-8/UTF-16 conversion and small ASCII utilities.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mwb::native {

[[nodiscard]] std::string ToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring ToWide(std::string_view utf8);

[[nodiscard]] bool EqualsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) noexcept;
[[nodiscard]] bool ContainsIgnoreCaseAscii(std::wstring_view text, std::wstring_view needle) noexcept;
[[nodiscard]] bool StartsWith(std::wstring_view text, std::wstring_view prefix) noexcept;

// True when `text` is made only of lowercase hex digits and its length is in [minLength, maxLength].
[[nodiscard]] bool IsLowerHex(std::wstring_view text, std::size_t minLength, std::size_t maxLength) noexcept;

// Cryptographically random lowercase hex string of 2 * `bytes` characters.
[[nodiscard]] std::wstring RandomHexToken(std::size_t bytes);

}  // namespace mwb::native
