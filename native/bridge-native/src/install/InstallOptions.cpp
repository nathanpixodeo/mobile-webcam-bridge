#include "install/InstallOptions.h"

#include "core/Errors.h"
#include "core/Strings.h"
#include "platform/OsVersion.h"

namespace mwb::native {

std::string_view ToString(CameraBackend backend) noexcept {
    switch (backend) {
        case CameraBackend::None: return "none";
        case CameraBackend::MediaFoundation: return "mf";
        case CameraBackend::DirectShow: return "dshow";
    }
    return "none";
}

std::optional<CameraBackend> ParseCameraBackend(std::wstring_view text) noexcept {
    if (text == L"mf") return CameraBackend::MediaFoundation;
    if (text == L"dshow") return CameraBackend::DirectShow;
    if (text == L"none") return CameraBackend::None;
    return std::nullopt;
}

ArgParser InstallArgParser() {
    return ArgParser{
        {L"camera", true},
        {L"mic", false},
        {L"no-mic", false},
        {L"width", true},
        {L"height", true},
        {L"fps", true},
        {L"name", true},
        {L"elevated", false, true},
        {L"result-pipe", true, true},
    };
}

ArgParser UninstallArgParser() {
    return ArgParser{
        {L"camera", false},
        {L"mic", false},
        {L"elevated", false, true},
        {L"result-pipe", true, true},
    };
}

namespace {

bool IsValidFriendlyName(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    for (const wchar_t c : name) {
        if (c < 0x20 || c == 0x7F || c == L'"' || c == L'\\') return false;
    }
    return true;
}

}  // namespace

InstallOptions ToInstallOptions(const ParsedOptions& options) {
    InstallOptions result;

    if (const std::optional<std::wstring> camera = options.Value(L"camera")) {
        if (*camera == L"auto") result.camera = CameraChoice::Auto;
        else if (*camera == L"mf") result.camera = CameraChoice::MediaFoundation;
        else if (*camera == L"dshow") result.camera = CameraChoice::DirectShow;
        else if (*camera == L"none") result.camera = CameraChoice::None;
        else throw CommandError(ErrorCode::Usage, "--camera expects auto, mf, dshow or none");
    }

    if (options.Has(L"mic") && options.Has(L"no-mic")) {
        throw CommandError(ErrorCode::Usage, "--mic and --no-mic are mutually exclusive");
    }
    result.mic = !options.Has(L"no-mic");

    const std::optional<std::uint32_t> width = options.UInt32(L"width", 1, mwb::frame::kMaxWidth);
    const std::optional<std::uint32_t> height = options.UInt32(L"height", 1, mwb::frame::kMaxHeight);
    if (width.has_value() != height.has_value()) {
        throw CommandError(ErrorCode::Usage, "--width and --height must be given together");
    }
    if (width) {
        result.mode.width = *width;
        result.mode.height = *height;
    }
    if (const std::optional<std::uint32_t> fps = options.UInt32(L"fps", 1, 240)) result.mode.fpsNum = *fps;
    result.mode.fpsDen = 1;
    if (!mwb::frame::IsSupportedMode(result.mode)) {
        throw CommandError(ErrorCode::Usage,
                           "unsupported camera mode; sizes: 640x360, 1280x720, 1920x1080; fps: 15, 24, 25, 30, 60");
    }

    if (const std::optional<std::wstring> name = options.Value(L"name")) {
        if (!IsValidFriendlyName(*name)) {
            throw CommandError(ErrorCode::Usage, "--name must be 1-64 printable characters without quotes or backslashes");
        }
        result.friendlyName = *name;
    }
    return result;
}

UninstallOptions ToUninstallOptions(const ParsedOptions& options) {
    const bool camera = options.Has(L"camera");
    const bool mic = options.Has(L"mic");
    if (!camera && !mic) return UninstallOptions{};  // no flags: remove everything
    return UninstallOptions{camera, mic};
}

CameraBackend ResolveCameraBackend(CameraChoice choice, const OsVersion& os) {
    switch (choice) {
        case CameraChoice::None: return CameraBackend::None;
        case CameraChoice::DirectShow: return CameraBackend::DirectShow;
        case CameraChoice::MediaFoundation:
            if (!os.IsWindows11OrLater()) {
                throw CommandError(ErrorCode::NotSupportedOs,
                                   "The Media Foundation virtual camera needs Windows 11 (build 22000 or later); this is build " +
                                       std::to_string(os.build));
            }
            return CameraBackend::MediaFoundation;
        case CameraChoice::Auto:
            return os.IsWindows11OrLater() ? CameraBackend::MediaFoundation : CameraBackend::DirectShow;
    }
    return CameraBackend::None;
}

}  // namespace mwb::native
