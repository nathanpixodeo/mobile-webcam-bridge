#include "video/HubSettings.h"

#include "core/Errors.h"
#include "core/Strings.h"
#include "install/ProductRegistry.h"

namespace mwb::native {

ArgParser HubArgParser() {
    return ArgParser{
        {L"ingest-pipe", true}, {L"pipe-name", true},  {L"width", true},      {L"height", true},  {L"fps-num", true},
        {L"fps-den", true},     {L"max-width", true}, {L"max-height", true}, {L"max-fps", true},
    };
}

ArgParser WatchArgParser() {
    return ArgParser{{L"frames", true}, {L"timeout-ms", true}, {L"pipe-name", true},
                     {L"width", true},  {L"height", true},     {L"fps", true}};
}

bool IsValidIngestPipePath(std::wstring_view path) noexcept {
    const std::wstring prefix = std::wstring(mwb::frame::kPipeNamespace) + mwb::frame::kIngestPipePrefix;
    return StartsWith(path, prefix) && IsLowerHex(path.substr(prefix.size()), 16, 64);
}

namespace {

std::string Describe(const mwb::frame::VideoMode& mode) {
    return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" + std::to_string(mode.fpsNum) + "/" +
           std::to_string(mode.fpsDen);
}

// --width and --height, both or neither.
std::optional<mwb::frame::FrameSize> SizeOption(const ParsedOptions& options, const wchar_t* widthName,
                                                const wchar_t* heightName) {
    const std::optional<std::uint32_t> width = options.UInt32(widthName, 1, mwb::frame::kMaxWidth);
    const std::optional<std::uint32_t> height = options.UInt32(heightName, 1, mwb::frame::kMaxHeight);
    if (width.has_value() != height.has_value()) {
        throw CommandError(ErrorCode::Usage, "--" + ToUtf8(widthName) + " and --" + ToUtf8(heightName) +
                                                 " must be given together");
    }
    if (!width) return std::nullopt;
    return mwb::frame::FrameSize{*width, *height};
}

std::wstring ResolvePipeName(const ParsedOptions& options, const std::optional<CameraRegistration>& camera) {
    std::wstring name = options.Value(L"pipe-name").value_or(camera ? camera->pipeName : mwb::frame::kDefaultPublicPipeName);
    if (!IsValidPipeName(name)) throw CommandError(ErrorCode::Usage, "--pipe-name must match [A-Za-z0-9._-]{1,128}");
    return name;
}

}  // namespace

HubSettings ToHubSettings(const ParsedOptions& options) {
    HubSettings settings;
    settings.ingestPipePath = options.Required(L"ingest-pipe");
    if (!IsValidIngestPipePath(settings.ingestPipePath)) {
        throw CommandError(ErrorCode::Usage, "--ingest-pipe must be \\\\.\\pipe\\mobile-webcam-bridge-ingest-<16-64 lowercase hex>");
    }

    const std::optional<CameraRegistration> camera = ProductRegistry{}.ReadCamera();
    settings.publicPipeName = ResolvePipeName(options, camera);

    const std::optional<mwb::frame::FrameSize> size = SizeOption(options, L"width", L"height");
    if (!size && !camera) {
        throw CommandError(ErrorCode::NotInstalled,
                           "The camera is not installed; pass --width and --height to run the hub without it");
    }

    mwb::frame::VideoMode mode = camera ? camera->defaultMode : mwb::frame::VideoMode{0, 0, 30, 1};
    if (size) {
        mode.width = size->width;
        mode.height = size->height;
    }
    if (const std::optional<std::uint32_t> fpsNum = options.UInt32(L"fps-num", 1, 240)) mode.fpsNum = *fpsNum;
    if (const std::optional<std::uint32_t> fpsDen = options.UInt32(L"fps-den", 1, 1001)) mode.fpsDen = *fpsDen;
    if (!mwb::frame::IsSupportedMode(mode)) throw CommandError(ErrorCode::Usage, "Unsupported camera mode " + Describe(mode));

    mwb::frame::ModeCap cap = camera ? camera->cap : mwb::frame::kFullCatalogCap;
    if (const std::optional<mwb::frame::FrameSize> maxSize = SizeOption(options, L"max-width", L"max-height")) {
        cap.maxWidth = maxSize->width;
        cap.maxHeight = maxSize->height;
    }
    if (const std::optional<std::uint32_t> maxFps = options.UInt32(L"max-fps", 1, 240)) cap.maxFps = *maxFps;
    if (!mwb::frame::IsValidCap(cap) || !mwb::frame::Admits(cap, mode)) {
        throw CommandError(ErrorCode::Usage, "The cap must use 15, 30 or 60 fps and admit the default mode " + Describe(mode));
    }

    settings.defaultMode = mode;
    settings.cap = cap;
    return settings;
}

WatchSettings ToWatchSettings(const ParsedOptions& options) {
    WatchSettings settings;
    const std::optional<CameraRegistration> camera = ProductRegistry{}.ReadCamera();
    settings.publicPipeName = ResolvePipeName(options, camera);
    settings.frames = options.UInt32(L"frames", 1, 100'000).value_or(30);
    settings.timeoutMs = options.UInt32(L"timeout-ms", 100, 600'000).value_or(5000);
    if (camera) settings.mode = camera->defaultMode;
    if (const std::optional<mwb::frame::FrameSize> size = SizeOption(options, L"width", L"height")) {
        settings.mode.width = size->width;
        settings.mode.height = size->height;
    }
    if (const std::optional<std::uint32_t> fps = options.UInt32(L"fps", 1, 240)) settings.mode.fpsNum = *fps;
    if (!mwb::frame::IsSupportedMode(settings.mode)) {
        throw CommandError(ErrorCode::Usage, "Unsupported camera mode " + Describe(settings.mode));
    }
    return settings;
}

}  // namespace mwb::native
