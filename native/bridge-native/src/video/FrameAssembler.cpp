#include "video/FrameAssembler.h"

#include <algorithm>
#include <stdexcept>

namespace mwb::native {

FrameAssembler::FrameAssembler(std::size_t frameBytes, BufferSource bufferSource)
    : frameBytes_(frameBytes), bufferSource_(std::move(bufferSource)) {
    if (frameBytes_ == 0) throw std::invalid_argument("FrameAssembler: frame size must be positive");
    if (!bufferSource_) bufferSource_ = [] { return Buffer(); };
}

void FrameAssembler::EnsureBuffer() {
    if (current_.size() == frameBytes_) return;
    current_ = bufferSource_();
    current_.resize(frameBytes_);
}

std::span<std::uint8_t> FrameAssembler::WritableRegion() {
    EnsureBuffer();
    return std::span<std::uint8_t>(current_).subspan(filled_);
}

std::optional<FrameAssembler::Buffer> FrameAssembler::Commit(std::size_t count) {
    if (count > frameBytes_ - filled_) throw std::out_of_range("FrameAssembler: commit beyond the frame");
    filled_ += count;
    if (filled_ < frameBytes_) return std::nullopt;
    filled_ = 0;
    Buffer frame = std::move(current_);
    current_ = Buffer();
    return frame;
}

}  // namespace mwb::native
