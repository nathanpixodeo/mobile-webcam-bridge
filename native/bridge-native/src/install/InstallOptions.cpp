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
        {L"max-width", true},
        {L"max-height", true},
        {L"max-fps", true},
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

// "640x360, 640x480, …; fps 15, 30, 60 (3840x2160: up to 30)" from the catalog itself.
std::string CatalogDescription() {
    std::string sizes;
    for (auto it = mwb::frame::kCatalogSizes.rbegin(); it != mwb::frame::kCatalogSizes.rend(); ++it) {
        if (!sizes.empty()) sizes += ", ";
        sizes += std::to_string(it->width) + "x" + std::to_string(it->height);
    }
    return "sizes " + sizes + "; fps 15, 30, 60 (above 2560x1440: up to " +
           std::to_string(mwb::frame::kMaxFpsAbove1440p) + ")";
}

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
        result.defaultMode.width = *width;
        result.defaultMode.height = *height;
    }
    if (const std::optional<std::uint32_t> fps = options.UInt32(L"fps", 1, 240)) result.defaultMode.fpsNum = *fps;
    result.defaultMode.fpsDen = 1;
    if (!mwb::frame::IsSupportedMode(result.defaultMode)) {
        throw CommandError(ErrorCode::Usage, "unsupported camera mode; " + CatalogDescription());
    }

    const std::optional<std::uint32_t> maxWidth = options.UInt32(L"max-width", 1, mwb::frame::kMaxWidth);
    const std::optional<std::uint32_t> maxHeight = options.UInt32(L"max-height", 1, mwb::frame::kMaxHeight);
    if (maxWidth.has_value() != maxHeight.has_value()) {
        throw CommandError(ErrorCode::Usage, "--max-width and --max-height must be given together");
    }
    if (maxWidth) {
        result.cap.maxWidth = *maxWidth;
        result.cap.maxHeight = *maxHeight;
    }
    if (const std::optional<std::uint32_t> maxFps = options.UInt32(L"max-fps", 1, 240)) result.cap.maxFps = *maxFps;
    if (!mwb::frame::IsValidCap(result.cap)) {
        throw CommandError(ErrorCode::Usage, "--max-fps must be 15, 30 or 60 and the cap at least 640x360");
    }
    if (!mwb::frame::Admits(result.cap, result.defaultMode)) {
        throw CommandError(ErrorCode::Usage, "the default camera mode (--width/--height/--fps) exceeds the cap (--max-*)");
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
