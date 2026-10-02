// `bridge-native video watch`: connects to the public pipe like a camera component would,
// subscribes to one mode, validates every header and counts frames (diagnostics and integration
// tests).
#pragma once

#include <cstdint>

#include "video/HubSettings.h"

namespace mwb::native {

struct WatchReport {
    std::uint32_t frames = 0;
    std::uint32_t placeholderFrames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t elapsedMs = 0;
};

class FrameWatcher {
public:
    explicit FrameWatcher(WatchSettings settings) : settings_(std::move(settings)) {}

    // Throws CommandError (IoError) on timeout, invalid frames or when no hub is running.
    [[nodiscard]] WatchReport Run() const;

private:
    WatchSettings settings_;
};

}  // namespace mwb::native
