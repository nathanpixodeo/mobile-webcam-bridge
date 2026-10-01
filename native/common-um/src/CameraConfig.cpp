#include <mwb/um/CameraConfig.h>

#include <mwb/Identifiers.h>
#include <mwb/um/Tracing.h>

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstdint>
#include <optional>

namespace mwb::um {

namespace {

constexpr std::size_t kMaxPipeNameLength = 128;
constexpr std::size_t kMaxFriendlyNameLength = 64;

std::optional<DWORD> ReadDword(HKEY key, const wchar_t* valueName) noexcept {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

// Reads a REG_SZ of at most `maxChars` characters; longer values are rejected, not truncated.
std::optional<std::wstring> ReadString(HKEY key, const wchar_t* valueName, std::size_t maxChars) {
    wchar_t buffer[kMaxPipeNameLength + 1]{};
    static_assert(kMaxFriendlyNameLength <= kMaxPipeNameLength);
    DWORD size = static_cast<DWORD>((maxChars + 1) * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return std::nullopt;  // missing, wrong type, or too long (ERROR_MORE_DATA)
    }
    return std::wstring(buffer);
}

}  // namespace

bool IsValidPipeName(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > kMaxPipeNameLength) return false;
    for (const wchar_t c : name) {
        const bool allowed = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                             c == L'.' || c == L'_' || c == L'-';
        if (!allowed) return false;
    }
    return true;
}

std::wstring PipePath(std::wstring_view name) {
    std::wstring path(frame::kPipeNamespace);
    path.append(name);
    return path;
}

HRESULT LoadCameraSettings(CameraSettings& settings) noexcept try {
    settings = CameraSettings{};

    wil::unique_hkey key;
    // KEY_WOW64_64KEY: the x86 DirectShow filter must see the same key as the x64 components.
    const LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ids::kCameraRegistryKey, 0,
                                         KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put());
    if (status == ERROR_FILE_NOT_FOUND) return S_FALSE;
    RETURN_IF_WIN32_ERROR(status);

    frame::VideoMode mode = settings.mode;
    if (const auto width = ReadDword(key.get(), L"Width")) mode.width = *width;
    if (const auto height = ReadDword(key.get(), L"Height")) mode.height = *height;
    if (const auto fpsNum = ReadDword(key.get(), L"FpsNum")) mode.fpsNum = *fpsNum;
    if (const auto fpsDen = ReadDword(key.get(), L"FpsDen")) mode.fpsDen = *fpsDen;
    if (frame::IsSupportedMode(mode)) {
        settings.mode = mode;
    } else {
        trace::Writef(trace::Level::Warning, L"Ignoring unsupported camera mode {}x{}@{}/{} from the registry",
                      mode.width, mode.height, mode.fpsNum, mode.fpsDen);
    }

    if (auto pipeName = ReadString(key.get(), L"PipeName", kMaxPipeNameLength)) {
        if (IsValidPipeName(*pipeName)) {
            settings.pipeName = std::move(*pipeName);
        } else {
            trace::Write(trace::Level::Warning, L"Ignoring invalid PipeName from the registry");
        }
    }

    if (auto friendlyName = ReadString(key.get(), L"FriendlyName", kMaxFriendlyNameLength)) {
        if (!friendlyName->empty()) settings.friendlyName = std::move(*friendlyName);
    }
    return S_OK;
}
CATCH_RETURN()

}  // namespace mwb::um
