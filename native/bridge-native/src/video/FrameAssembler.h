// Slices the raw NV12 byte stream from the ingest pipe into whole frames. Pure logic: the caller
// reads straight into WritableRegion() (no intermediate copy) and commits what it read.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace mwb::native {

class FrameAssembler {
public:
    using Buffer = std::vector<std::uint8_t>;
    // Supplies an empty buffer to assemble the next frame into (lets the hub recycle buffers).
    using BufferSource = std::function<Buffer()>;

    FrameAssembler(std::size_t frameBytes, BufferSource bufferSource);

    // Unfilled tail of the frame being assembled; never empty.
    [[nodiscard]] std::span<std::uint8_t> WritableRegion();

    // Marks `count` bytes of WritableRegion() as filled. Returns the frame once it is complete.
    [[nodiscard]] std::optional<Buffer> Commit(std::size_t count);

    // Copies `data` in, calling `onFrame(Buffer&&)` for every completed frame.
    template <typename OnFrame>
    void Feed(std::span<const std::uint8_t> data, OnFrame&& onFrame) {
        while (!data.empty()) {
            const std::span<std::uint8_t> region = WritableRegion();
            const std::size_t count = region.size() < data.size() ? region.size() : data.size();
            std::copy_n(data.begin(), count, region.begin());
            data = data.subspan(count);
            if (std::optional<Buffer> frame = Commit(count)) onFrame(std::move(*frame));
        }
    }

    // Drops the partially assembled frame (e.g. when the writer disconnects).
    void Reset() noexcept { filled_ = 0; }

    [[nodiscard]] std::size_t FrameBytes() const noexcept { return frameBytes_; }
    [[nodiscard]] std::size_t PendingBytes() const noexcept { return filled_; }

private:
    void EnsureBuffer();

    std::size_t frameBytes_;
    BufferSource bufferSource_;
    Buffer current_;
    std::size_t filled_ = 0;
};

}  // namespace mwb::native
