// Recycles frame-sized buffers. Frames are 1.4-3 MB; allocating them fresh 30-60 times per
// second would commit and zero new pages for every frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mwb::native {

using FrameBytes = std::vector<std::uint8_t>;
using SharedFrameBytes = std::shared_ptr<const FrameBytes>;

class FramePool : public std::enable_shared_from_this<FramePool> {
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    [[nodiscard]] static std::shared_ptr<FramePool> Create(std::size_t frameBytes, std::size_t maxCached);

    FramePool(PrivateTag, std::size_t frameBytes, std::size_t maxCached) noexcept
        : frameBytes_(frameBytes), maxCached_(maxCached) {}

    // An empty buffer with capacity for one frame, reused when available.
    [[nodiscard]] FrameBytes Acquire();
    // Returns a buffer to the pool directly.
    void Release(FrameBytes buffer);
    // Shares a filled buffer; it returns to the pool once the last reference is gone.
    [[nodiscard]] SharedFrameBytes Share(FrameBytes buffer);

private:
    std::size_t frameBytes_;
    std::size_t maxCached_;
    std::mutex mutex_;
    std::vector<FrameBytes> cache_;
};

}  // namespace mwb::native
