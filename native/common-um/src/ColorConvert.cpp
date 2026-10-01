#include <mwb/um/ColorConvert.h>

#include <cstring>

namespace mwb::um::color {

namespace {

// BT.709 limited range, 8.8 fixed point:
//   R = 1.164(Y−16)              + 1.793(V−128)
//   G = 1.164(Y−16) − 0.213(U−128) − 0.533(V−128)
//   B = 1.164(Y−16) + 2.112(U−128)
constexpr int kLuma = 298;      // 1.164 × 256
constexpr int kVToR = 459;      // 1.793 × 256
constexpr int kUToG = 55;       // 0.213 × 256
constexpr int kVToG = 136;      // 0.533 × 256
constexpr int kUToB = 541;      // 2.112 × 256
constexpr int kRounding = 128;

constexpr std::uint8_t Clamp8(int value) noexcept {
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

}  // namespace

Nv12Image PackedNv12(const std::uint8_t* data, std::uint32_t width, std::uint32_t height) noexcept {
    return Nv12Image{
        .y = data,
        .uv = data + static_cast<std::size_t>(width) * height,
        .width = width,
        .height = height,
        .yStride = width,
        .uvStride = width,
    };
}

Rgb YuvToRgb(std::uint8_t y, std::uint8_t u, std::uint8_t v) noexcept {
    const int c = kLuma * (static_cast<int>(y) - 16) + kRounding;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    return Rgb{
        .r = Clamp8((c + kVToR * e) >> 8),
        .g = Clamp8((c - kUToG * d - kVToG * e) >> 8),
        .b = Clamp8((c + kUToB * d) >> 8),
    };
}

void CopyNv12(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept {
    const std::size_t rowBytes = src.width;
    std::uint8_t* dstUv = dst + dstPitch * src.height;

    if (src.yStride == rowBytes && dstPitch == rowBytes) {
        std::memcpy(dst, src.y, rowBytes * src.height);
    } else {
        for (std::uint32_t row = 0; row < src.height; ++row) {
            std::memcpy(dst + row * dstPitch, src.y + row * src.yStride, rowBytes);
        }
    }

    const std::uint32_t chromaRows = src.height / 2;
    if (src.uvStride == rowBytes && dstPitch == rowBytes) {
        std::memcpy(dstUv, src.uv, rowBytes * chromaRows);
    } else {
        for (std::uint32_t row = 0; row < chromaRows; ++row) {
            std::memcpy(dstUv + row * dstPitch, src.uv + row * src.uvStride, rowBytes);
        }
    }
}

void Nv12ToYuy2(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept {
    for (std::uint32_t row = 0; row < src.height; ++row) {
        const std::uint8_t* y = src.y + row * src.yStride;
        const std::uint8_t* uv = src.uv + (row / 2) * src.uvStride;
        std::uint8_t* out = dst + row * dstPitch;
        for (std::uint32_t x = 0; x < src.width; x += 2) {
            out[0] = y[x];
            out[1] = uv[x];      // U
            out[2] = y[x + 1];
            out[3] = uv[x + 1];  // V
            out += 4;
        }
    }
}

void Nv12ToI420(const Nv12Image& src, std::uint8_t* dst, std::size_t yPitch) noexcept {
    const std::size_t chromaPitch = yPitch / 2;
    const std::uint32_t chromaWidth = src.width / 2;
    const std::uint32_t chromaRows = src.height / 2;
    std::uint8_t* planeU = dst + yPitch * src.height;
    std::uint8_t* planeV = planeU + chromaPitch * chromaRows;

    for (std::uint32_t row = 0; row < src.height; ++row) {
        std::memcpy(dst + row * yPitch, src.y + row * src.yStride, src.width);
    }
    for (std::uint32_t row = 0; row < chromaRows; ++row) {
        const std::uint8_t* uv = src.uv + row * src.uvStride;
        std::uint8_t* u = planeU + row * chromaPitch;
        std::uint8_t* v = planeV + row * chromaPitch;
        for (std::uint32_t x = 0; x < chromaWidth; ++x) {
            u[x] = uv[2 * x];
            v[x] = uv[2 * x + 1];
        }
    }
}

void Nv12ToRgb24BottomUp(const Nv12Image& src, std::uint8_t* dst, std::size_t dstPitch) noexcept {
    for (std::uint32_t row = 0; row < src.height; ++row) {
        const std::uint8_t* y = src.y + row * src.yStride;
        const std::uint8_t* uv = src.uv + (row / 2) * src.uvStride;
        std::uint8_t* out = dst + static_cast<std::size_t>(src.height - 1 - row) * dstPitch;
        for (std::uint32_t x = 0; x < src.width; x += 2) {
            const int d = static_cast<int>(uv[x]) - 128;
            const int e = static_cast<int>(uv[x + 1]) - 128;
            const int chromaR = kVToR * e;
            const int chromaG = -kUToG * d - kVToG * e;
            const int chromaB = kUToB * d;
            for (std::uint32_t i = 0; i < 2; ++i) {
                const int c = kLuma * (static_cast<int>(y[x + i]) - 16) + kRounding;
                out[0] = Clamp8((c + chromaB) >> 8);  // DIB pixel order is B, G, R
                out[1] = Clamp8((c + chromaG) >> 8);
                out[2] = Clamp8((c + chromaR) >> 8);
                out += 3;
            }
        }
    }
}

}  // namespace mwb::um::color
