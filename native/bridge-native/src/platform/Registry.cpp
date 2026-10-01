#include "platform/Registry.h"

#include <vector>

#include <wil/result.h>

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

namespace {

constexpr REGSAM ViewFlag(RegistryView view) noexcept {
    return view == RegistryView::Native64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
}

[[noreturn]] void ThrowRegistry(LSTATUS status, std::wstring_view what) {
    throw ErrorFromWin32(static_cast<unsigned long>(status), "Registry " + ToUtf8(what));
}

}  // namespace

std::optional<RegistryKey> RegistryKey::Open(HKEY root, std::wstring_view path, REGSAM access, RegistryView view) {
    const std::wstring subkey(path);
    wil::unique_hkey key;
    const LSTATUS status = ::RegOpenKeyExW(root, subkey.c_str(), 0, access | ViewFlag(view), &key);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    if (status != ERROR_SUCCESS) ThrowRegistry(status, path);
    return RegistryKey(std::move(key));
}

RegistryKey RegistryKey::Create(HKEY root, std::wstring_view path, RegistryView view) {
    const std::wstring subkey(path);
    wil::unique_hkey key;
    const LSTATUS status = ::RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                            KEY_READ | KEY_WRITE | ViewFlag(view), nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) ThrowRegistry(status, path);
    return RegistryKey(std::move(key));
}

bool RegistryKey::Exists(HKEY root, std::wstring_view path, RegistryView view) {
    return Open(root, path, KEY_QUERY_VALUE, view).has_value();
}

void RegistryKey::DeleteTree(HKEY root, std::wstring_view path, RegistryView view) {
    const std::wstring subkey(path);
    LSTATUS status = ERROR_SUCCESS;
    {
        wil::unique_hkey key;
        status = ::RegOpenKeyExW(root, subkey.c_str(), 0,
                                 DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | ViewFlag(view), &key);
        if (status == ERROR_FILE_NOT_FOUND) return;
        if (status != ERROR_SUCCESS) ThrowRegistry(status, path);
        status = ::RegDeleteTreeW(key.get(), nullptr);  // values and subkeys
        if (status != ERROR_SUCCESS) ThrowRegistry(status, path);
    }
    status = ::RegDeleteKeyExW(root, subkey.c_str(), ViewFlag(view), 0);  // the key itself
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) ThrowRegistry(status, path);
}

std::optional<std::wstring> RegistryKey::GetString(std::wstring_view name) const {
    const std::wstring valueName(name);
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = ::RegQueryValueExW(key_.get(), valueName.c_str(), nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    if (status != ERROR_SUCCESS) ThrowRegistry(status, name);
    if (type != REG_SZ && type != REG_EXPAND_SZ) return std::nullopt;

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    status = ::RegQueryValueExW(key_.get(), valueName.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                                &bytes);
    if (status != ERROR_SUCCESS) ThrowRegistry(status, name);
    std::wstring value(buffer.data(), bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::optional<DWORD> RegistryKey::GetDword(std::wstring_view name) const {
    const std::wstring valueName(name);
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status =
        ::RegQueryValueExW(key_.get(), valueName.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    if (status != ERROR_SUCCESS) ThrowRegistry(status, name);
    if (type != REG_DWORD || bytes != sizeof(value)) return std::nullopt;
    return value;
}

void RegistryKey::SetString(std::wstring_view name, std::wstring_view value) {
    const std::wstring valueName(name);
    const std::wstring data(value);
    const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    const LSTATUS status =
        ::RegSetValueExW(key_.get(), valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    if (status != ERROR_SUCCESS) ThrowRegistry(status, name);
}

void RegistryKey::SetDword(std::wstring_view name, DWORD value) {
    const std::wstring valueName(name);
    const LSTATUS status =
        ::RegSetValueExW(key_.get(), valueName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status != ERROR_SUCCESS) ThrowRegistry(status, name);
}

}  // namespace mwb::native
