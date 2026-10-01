// Compile-time GUID parsing, so the textual CLSIDs in mwb/Identifiers.h stay the single source of
// truth for both registration (strings) and COM lookups (GUID values).
#pragma once

#include <guiddef.h>

namespace mwb::um {

namespace detail {

consteval unsigned HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return static_cast<unsigned>(c - L'0');
    if (c >= L'a' && c <= L'f') return static_cast<unsigned>(c - L'a' + 10);
    if (c >= L'A' && c <= L'F') return static_cast<unsigned>(c - L'A' + 10);
    throw "invalid hexadecimal digit in GUID literal";  // compile error when evaluated
}

template <typename T>
consteval T ParseHex(const wchar_t* text, int digits) {
    unsigned long long value = 0;
    for (int i = 0; i < digits; ++i) value = (value << 4) | HexDigit(text[i]);
    return static_cast<T>(value);
}

}  // namespace detail

// Parses "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
consteval GUID GuidFromString(const wchar_t (&text)[39]) {
    if (text[0] != L'{' || text[9] != L'-' || text[14] != L'-' || text[19] != L'-' || text[24] != L'-' ||
        text[37] != L'}') {
        throw "malformed GUID literal";
    }
    GUID guid{};
    guid.Data1 = detail::ParseHex<unsigned long>(text + 1, 8);
    guid.Data2 = detail::ParseHex<unsigned short>(text + 10, 4);
    guid.Data3 = detail::ParseHex<unsigned short>(text + 15, 4);
    guid.Data4[0] = detail::ParseHex<unsigned char>(text + 20, 2);
    guid.Data4[1] = detail::ParseHex<unsigned char>(text + 22, 2);
    for (int i = 0; i < 6; ++i) {
        guid.Data4[2 + i] = detail::ParseHex<unsigned char>(text + 25 + 2 * i, 2);
    }
    return guid;
}

}  // namespace mwb::um
