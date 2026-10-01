#include <mwb/um/Placeholder.h>

#include <algorithm>
#include <cstring>

namespace mwb::um {

namespace {
constexpr std::uint32_t kSweepSeconds = 4;
}

PlaceholderGenerator::PlaceholderGenerator(std::uint32_t width, std::uint32_t height, std::uint32_t fps) noexcept
    : width_(width),
      height_(height),
      // Even width keeps the bar aligned to NV12 chroma pairs.
      barWidth_(std::max<std::uint32_t>(8, (width / 24) & ~1u)),
      framesPerSweep_(std::max<std::uint32_t>(1, fps * kSweepSeconds)) {}

std::size_t PlaceholderGenerator::FrameBytes() const noexcept {
    return static_cast<std::size_t>(width_) * height_ * 3 / 2;
}

std::uint32_t PlaceholderGenerator::BarOffset(std::uint64_t frameIndex) const noexcept {
    if (width_ <= barWidth_) return 0;
    const std::uint64_t travel = width_ - barWidth_;
    const std::uint64_t step = frameIndex % framesPerSweep_;
    return static_cast<std::uint32_t>((travel * step / framesPerSweep_) & ~std::uint64_t{1});
}

void PlaceholderGenerator::Render(std::uint64_t frameIndex, std::uint8_t* nv12) const noexcept {
    const std::size_t lumaBytes = static_cast<std::size_t>(width_) * height_;
    std::memset(nv12, kBackgroundLuma, lumaBytes);
    std::memset(nv12 + lumaBytes, kNeutralChroma, lumaBytes / 2);

    const std::uint32_t offset = BarOffset(frameIndex);
    const std::uint32_t barWidth = std::min(barWidth_, width_ - offset);
    for (std::uint32_t row = 0; row < height_; ++row) {
        std::memset(nv12 + static_cast<std::size_t>(row) * width_ + offset, kBarLuma, barWidth);
    }
}

}  // namespace mwb::um
