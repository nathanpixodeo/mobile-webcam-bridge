// Options of `video hub` and `video watch`, validated before any pipe is created.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <mwb/FrameProtocol.h>

#include "core/ArgParser.h"

namespace mwb::native {

struct HubSettings {
    std::wstring ingestPipePath;     // \\.\pipe\mobile-webcam-bridge-ingest-<token>
    std::wstring publicPipeName;     // without the \\.\pipe\ prefix
    mwb::frame::VideoMode defaultMode{};  // initial ingest size; size of the neutral placeholder
    mwb::frame::ModeCap cap{mwb::frame::kFullCatalogCap};  // subscriptions must be within it
};

struct WatchSettings {
    std::wstring publicPipeName;
    std::uint32_t frames = 30;
    std::uint32_t timeoutMs = 5000;
    mwb::frame::VideoMode mode{1280, 720, 30, 1};  // the mode to subscribe to
};

[[nodiscard]] ArgParser HubArgParser();
[[nodiscard]] ArgParser WatchArgParser();

// Explicit options win; otherwise the installed camera modes and pipe name are used.
// Throws CommandError (Usage, NotInstalled).
[[nodiscard]] HubSettings ToHubSettings(const ParsedOptions& options);
[[nodiscard]] WatchSettings ToWatchSettings(const ParsedOptions& options);

[[nodiscard]] bool IsValidIngestPipePath(std::wstring_view path) noexcept;

}  // namespace mwb::native
