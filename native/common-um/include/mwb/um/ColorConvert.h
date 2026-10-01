// NV12 → other pixel formats for the camera consumers. Scalar but branch-light: these run once per
// delivered frame (≤ 1080p60), which is far below what a modern core sustains.
//
// Colorimetry: BT.709, limited range (Y 16–235, UV 16–240), matching protocol/FRAME_PIPE.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mwb::um::color {

// Read-only view of an NV12 image: a Y plane plus an interleaved UV plane at half resolution.
struct Nv12Image {
    const std::uint8_t* y = nullptr;
    const std::uint8_t* uv = nullptr;
    std::uint32_t width = 0;   // even
    std::uint32_t height = 0;  // even
    std::size_t yStride = 0;
    std::size_t uvStride = 0;
};

// View over a tightly packed NV12 buffer (UV plane immediately follows the Y plane).
[[nodiscard]] Nv12Image PackedNv12(const std::uint8_t* data, std::uint32_t width, std::uint32_t height) noexcept;

// NV12 with any destination pitch; the destination UV plane starts at dst + dstPitch * height.
void CopyNv12(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept;

// Packed 4:2:2 YUY2 (Y0 U Y1 V); each chroma row is used for two output rows.
void Nv12ToYuy2(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept;

// Planar 4:2:0 I420: Y plane (pitch `yPitch`), then U, then V (pitch `yPitch / 2` each),
// each plane directly following the previous one.
void Nv12ToI420(const Nv12Image& src, std::uint8_t* dst, std::size_t yPitch) noexcept;

// 24-bit BGR, bottom-up DIB layout (the first row in memory is the bottom image row).
void Nv12ToRgb24BottomUp(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept;

// DIB row size of a 24-bit image (rows are DWORD aligned).
[[nodiscard]] constexpr std::size_t Rgb24Pitch(std::uint32_t width) noexcept {
    return (static_cast<std::size_t>(width) * 3u + 3u) & ~static_cast<std::size_t>(3u);
}

// One pixel, exposed for tests: converts limited-range BT.709 YUV to 8-bit RGB.
struct Rgb {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};
[[nodiscard]] Rgb YuvToRgb(std::uint8_t y, std::uint8_t u, std::uint8_t v) noexcept;

}  // namespace mwb::um::color
