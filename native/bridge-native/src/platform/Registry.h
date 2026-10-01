// Thin RAII wrapper over the registry with an explicit 32/64-bit view, so the x64 installer and
// the x86 DirectShow DLL always agree on which keys they touch.
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>

namespace mwb::native {

enum class RegistryView {
    Native64,  // KEY_WOW64_64KEY
    Wow32,     // KEY_WOW64_32KEY
};

class RegistryKey {
public:
    // nullopt when the key does not exist; throws on other errors.
    [[nodiscard]] static std::optional<RegistryKey> Open(HKEY root, std::wstring_view path, REGSAM access = KEY_READ,
                                                         RegistryView view = RegistryView::Native64);
    [[nodiscard]] static RegistryKey Create(HKEY root, std::wstring_view path,
                                            RegistryView view = RegistryView::Native64);
    [[nodiscard]] static bool Exists(HKEY root, std::wstring_view path, RegistryView view = RegistryView::Native64);
    // Deletes the key with all values and subkeys. Missing keys are not an error.
    static void DeleteTree(HKEY root, std::wstring_view path, RegistryView view = RegistryView::Native64);

    [[nodiscard]] std::optional<std::wstring> GetString(std::wstring_view name) const;
    [[nodiscard]] std::optional<DWORD> GetDword(std::wstring_view name) const;
    void SetString(std::wstring_view name, std::wstring_view value);
    void SetDword(std::wstring_view name, DWORD value);

    [[nodiscard]] HKEY Get() const noexcept { return key_.get(); }

private:
    explicit RegistryKey(wil::unique_hkey key) noexcept : key_(std::move(key)) {}

    wil::unique_hkey key_;
};

}  // namespace mwb::native
