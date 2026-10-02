// Types shared by the video hub components.
#pragma once

#include <atomic>
#include <cstdint>

#include <mwb/FrameProtocol.h>

#include "video/FramePool.h"

namespace mwb::native {

// One frame the hub publishes (live or placeholder), at its own size. Each consumer turns it into
// a frame message of its subscribed size (scaling when the sizes differ) and adds FORMAT_CHANGED
// to the first one it sends.
struct OutgoingFrame {
    SharedFrameBytes payload;  // tightly packed NV12 of width × height
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t flags = 0;   // frame::kFlagPlaceholder or 0
    std::uint64_t seq = 0;
    std::uint64_t producerQpc100ns = 0;
};

// Cumulative counters reported in the hub's "stats" events.
struct HubCounters {
    std::atomic<std::uint64_t> framesIn{0};
    std::atomic<std::uint64_t> framesOut{0};
    std::atomic<std::uint64_t> consumerDrops{0};
    std::atomic<std::uint64_t> placeholderFrames{0};
};

}  // namespace mwb::native
