// Frame shown by a camera consumer while no live frame source is reachable (the hub is not
// running). A dark neutral picture with a slowly sweeping bar: unmistakably "no signal", and the
// motion makes frame pacing problems visible during testing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mwb::um {

class PlaceholderGenerator final {
public:
    PlaceholderGenerator(std::uint32_t width, std::uint32_t height, std::uint32_t fps) noexcept;

    // Bytes of one tightly packed NV12 frame.
    [[nodiscard]] std::size_t FrameBytes() const noexcept;

    // Writes frame `frameIndex` as tightly packed NV12 into `nv12` (FrameBytes() bytes).
    void Render(std::uint64_t frameIndex, std::uint8_t* nv12) const noexcept;

    // Column where the bar starts for `frameIndex` (exposed for tests).
    [[nodiscard]] std::uint32_t BarOffset(std::uint64_t frameIndex) const noexcept;
    [[nodiscard]] std::uint32_t BarWidth() const noexcept { return barWidth_; }

    static constexpr std::uint8_t kBackgroundLuma = 24;  // limited-range black is 16
    static constexpr std::uint8_t kBarLuma = 72;
    static constexpr std::uint8_t kNeutralChroma = 128;

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t barWidth_;
    std::uint32_t framesPerSweep_;
};

}  // namespace mwb::um
