#include "video/HubSettings.h"

#include "core/Errors.h"
#include "core/Strings.h"
#include "install/ProductRegistry.h"

namespace mwb::native {

ArgParser HubArgParser() {
    return ArgParser{
        {L"ingest-pipe", true}, {L"pipe-name", true}, {L"width", true},
        {L"height", true},      {L"fps-num", true},   {L"fps-den", true},
    };
}

ArgParser WatchArgParser() {
    return ArgParser{{L"frames", true}, {L"timeout-ms", true}, {L"pipe-name", true}};
}

bool IsValidIngestPipePath(std::wstring_view path) noexcept {
    const std::wstring prefix = std::wstring(mwb::frame::kPipeNamespace) + mwb::frame::kIngestPipePrefix;
    return StartsWith(path, prefix) && IsLowerHex(path.substr(prefix.size()), 16, 64);
}

namespace {

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

    const std::optional<std::uint32_t> width = options.UInt32(L"width", 1, mwb::frame::kMaxWidth);
    const std::optional<std::uint32_t> height = options.UInt32(L"height", 1, mwb::frame::kMaxHeight);
    if (width.has_value() != height.has_value()) {
        throw CommandError(ErrorCode::Usage, "--width and --height must be given together");
    }
    if (!width && !camera) {
        throw CommandError(ErrorCode::NotInstalled,
                           "The camera is not installed; pass --width and --height to run the hub without it");
    }

    mwb::frame::VideoMode mode = camera ? camera->mode : mwb::frame::VideoMode{0, 0, 30, 1};
    if (width) {
        mode.width = *width;
        mode.height = *height;
    }
    if (const std::optional<std::uint32_t> fpsNum = options.UInt32(L"fps-num", 1, 240)) mode.fpsNum = *fpsNum;
    if (const std::optional<std::uint32_t> fpsDen = options.UInt32(L"fps-den", 1, 1001)) mode.fpsDen = *fpsDen;
    if (!mwb::frame::IsSupportedMode(mode)) {
        throw CommandError(ErrorCode::Usage, "Unsupported camera mode " + std::to_string(mode.width) + "x" +
                                                 std::to_string(mode.height) + "@" + std::to_string(mode.fpsNum) + "/" +
                                                 std::to_string(mode.fpsDen));
    }
    settings.mode = mode;
    return settings;
}

WatchSettings ToWatchSettings(const ParsedOptions& options) {
    WatchSettings settings;
    const std::optional<CameraRegistration> camera = ProductRegistry{}.ReadCamera();
    settings.publicPipeName = ResolvePipeName(options, camera);
    settings.frames = options.UInt32(L"frames", 1, 100'000).value_or(30);
    settings.timeoutMs = options.UInt32(L"timeout-ms", 100, 600'000).value_or(5000);
    if (camera) settings.expectedMode = camera->mode;
    return settings;
}

}  // namespace mwb::native
