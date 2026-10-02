// Resizes NV12 frames for the video hub, which serves every consumer at the size it subscribed to
// (protocol/FRAME_PIPE.md §3.2). The whole source stays visible: when the aspect ratios differ the
// image is fitted and letterboxed with black bars (Y 16, UV 128).
//
// Downscaling by 2× or more first halves the image with a 2×2 box filter (repeatedly), then a
// bilinear pass covers the remaining ratio; bilinear alone would alias badly at 4K → 360p.
// Upscaling is bilinear. Scalar, fixed point; one instance per source/destination size pair.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <mwb/um/ColorConvert.h>

namespace mwb::um::color {

class Nv12Scaler {
public:
    // All sizes even and non-zero (NV12 subsampling); the caller validates them.
    Nv12Scaler(std::uint32_t srcWidth, std::uint32_t srcHeight, std::uint32_t dstWidth, std::uint32_t dstHeight);

    // Writes a tightly packed dstWidth × dstHeight NV12 frame (Nv12FrameBytes bytes) to `dst`.
    // `src` must have the source size given to the constructor. Not thread-safe (scratch buffers).
    void Scale(const Nv12Image& src, std::uint8_t* dst);

    [[nodiscard]] std::uint32_t SourceWidth() const noexcept { return srcWidth_; }
    [[nodiscard]] std::uint32_t SourceHeight() const noexcept { return srcHeight_; }
    [[nodiscard]] std::uint32_t DestinationWidth() const noexcept { return dstWidth_; }
    [[nodiscard]] std::uint32_t DestinationHeight() const noexcept { return dstHeight_; }

    // Where the image lands inside the destination (the rest is bars); even values. For tests.
    struct Rect {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t width;
        std::uint32_t height;
    };
    [[nodiscard]] const Rect& Content() const noexcept { return content_; }

private:
    // Sample positions along one axis of the bilinear pass: source index pair and the weight of
    // the second sample in 1/256 units.
    struct Tap {
        std::uint32_t first;
        std::uint32_t second;
        std::uint32_t weight;
    };

    static std::vector<Tap> MakeTaps(std::uint32_t srcSize, std::uint32_t dstSize);

    std::uint32_t srcWidth_;
    std::uint32_t srcHeight_;
    std::uint32_t dstWidth_;
    std::uint32_t dstHeight_;
    Rect content_{};

    std::uint32_t halvings_ = 0;            // 2×2 box passes before the bilinear pass
    std::uint32_t reducedWidth_ = 0;        // size after the box passes
    std::uint32_t reducedHeight_ = 0;
    std::vector<std::uint8_t> scratch_[2];  // ping-pong buffers for the box passes

    std::vector<Tap> lumaX_;
    std::vector<Tap> lumaY_;
    std::vector<Tap> chromaX_;
    std::vector<Tap> chromaY_;
};

}  // namespace mwb::um::color
