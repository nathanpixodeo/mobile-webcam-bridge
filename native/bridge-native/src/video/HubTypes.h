// Types shared by the video hub components.
#pragma once

#include <atomic>
#include <cstdint>

#include <mwb/FrameProtocol.h>

#include "video/FramePool.h"

namespace mwb::native {

// One frame as sent on the public pipe. The header is per frame; consumers that just connected
// get FORMAT_CHANGED added on their copy of it.
struct OutgoingFrame {
    mwb::frame::FrameHeader header{};
    SharedFrameBytes payload;
};

// Cumulative counters reported in the hub's "stats" events.
struct HubCounters {
    std::atomic<std::uint64_t> framesIn{0};
    std::atomic<std::uint64_t> framesOut{0};
    std::atomic<std::uint64_t> consumerDrops{0};
    std::atomic<std::uint64_t> placeholderFrames{0};
};

}  // namespace mwb::native
