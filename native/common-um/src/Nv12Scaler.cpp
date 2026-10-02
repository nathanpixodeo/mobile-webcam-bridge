#include <mwb/um/Nv12Scaler.h>

#include <algorithm>
#include <cstring>

namespace mwb::um::color {

namespace {

constexpr std::uint8_t kBlackLuma = 16;
constexpr std::uint8_t kNeutralChroma = 128;
constexpr std::uint32_t kWeightOne = 256;

// One plane of an NV12 image: `channels` interleaved bytes per pixel (1 for Y, 2 for UV).
struct ConstPlane {
    const std::uint8_t* data;
    std::size_t stride;
    std::uint32_t width;   // pixels
    std::uint32_t height;  // rows
};

struct MutablePlane {
    std::uint8_t* data;
    std::size_t stride;
};

constexpr std::uint32_t EvenDown(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & ~std::uint64_t{1});
}

// 2×2 box filter: dst has half the width and height of src.
template <std::uint32_t Channels>
void HalvePlane(const ConstPlane& src, const MutablePlane& dst) noexcept {
    const std::uint32_t width = src.width / 2;
    const std::uint32_t height = src.height / 2;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* top = src.data + (2 * row) * src.stride;
        const std::uint8_t* bottom = top + src.stride;
        std::uint8_t* out = dst.data + row * dst.stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t left = 2 * x * Channels;
            for (std::uint32_t c = 0; c < Channels; ++c) {
                const std::uint32_t sum = top[left + c] + top[left + Channels + c] + bottom[left + c] +
                                          bottom[left + Channels + c];
                out[x * Channels + c] = static_cast<std::uint8_t>((sum + 2) >> 2);
            }
        }
    }
}

// Horizontal pass over one source row; results keep 8 fractional bits (0..65280 fits 16 bits).
template <std::uint32_t Channels, typename Tap>
void HorizontalRow(const std::uint8_t* source, const std::vector<Tap>& tapsX, std::uint16_t* out) noexcept {
    for (std::size_t x = 0; x < tapsX.size(); ++x) {
        const Tap& tap = tapsX[x];
        const std::uint32_t weight = tap.weight;
        const std::uint32_t first = tap.first * Channels;
        const std::uint32_t second = tap.second * Channels;
        for (std::uint32_t c = 0; c < Channels; ++c) {
            out[x * Channels + c] =
                static_cast<std::uint16_t>(source[first + c] * (kWeightOne - weight) + source[second + c] * weight);
        }
    }
}

// Vertical blend of two horizontally scaled rows; a plain loop the compiler vectorises.
void VerticalRow(const std::uint16_t* top, const std::uint16_t* bottom, std::uint32_t weight, std::uint8_t* out,
                 std::size_t count) noexcept {
    const std::uint32_t topWeight = kWeightOne - weight;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>((top[i] * topWeight + bottom[i] * weight + 32768u) >> 16);
    }
}

// Separable bilinear: each source row is scaled horizontally once and kept while the destination
// rows that need it are blended (upscaling reuses every row for several destination rows).
template <std::uint32_t Channels, typename Tap>
void BilinearPlane(const ConstPlane& src, const MutablePlane& dst, const std::vector<Tap>& tapsX,
                   const std::vector<Tap>& tapsY, std::vector<std::uint16_t> (&rows)[2]) {
    const std::size_t count = tapsX.size() * Channels;
    rows[0].resize(count);
    rows[1].resize(count);
    std::int64_t cached[2] = {-1, -1};  // source row held by each buffer

    // The buffer holding source row `index`, filling the one that is not `keep` when needed.
    const auto rowFor = [&](std::uint32_t index, int keep) -> int {
        for (int slot = 0; slot < 2; ++slot) {
            if (cached[slot] == index) return slot;
        }
        const int slot = keep == 0 ? 1 : 0;
        HorizontalRow<Channels>(src.data + index * src.stride, tapsX, rows[slot].data());
        cached[slot] = index;
        return slot;
    };

    for (std::size_t row = 0; row < tapsY.size(); ++row) {
        const Tap& ty = tapsY[row];
        const int top = rowFor(ty.first, -1);
        const int bottom = ty.weight == 0 ? top : rowFor(ty.second, top);
        VerticalRow(rows[top].data(), rows[bottom].data(), ty.weight, dst.data + row * dst.stride, count);
    }
}

void CopyPlane(const ConstPlane& src, std::size_t rowBytes, const MutablePlane& dst) noexcept {
    for (std::uint32_t row = 0; row < src.height; ++row) {
        std::memcpy(dst.data + row * dst.stride, src.data + row * src.stride, rowBytes);
    }
}

// Fills everything of a plane outside the content rectangle (all values in bytes and rows).
void FillBars(std::uint8_t* plane, std::size_t stride, std::size_t rows, std::size_t contentX, std::size_t contentY,
              std::size_t contentBytes, std::size_t contentRows, std::uint8_t value) noexcept {
    for (std::size_t row = 0; row < rows; ++row) {
        std::uint8_t* line = plane + row * stride;
        if (row < contentY || row >= contentY + contentRows) {
            std::memset(line, value, stride);
        } else {
            std::memset(line, value, contentX);
            std::memset(line + contentX + contentBytes, value, stride - contentX - contentBytes);
        }
    }
}

}  // namespace

Nv12Scaler::Nv12Scaler(std::uint32_t srcWidth, std::uint32_t srcHeight, std::uint32_t dstWidth, std::uint32_t dstHeight)
    : srcWidth_(srcWidth), srcHeight_(srcHeight), dstWidth_(dstWidth), dstHeight_(dstHeight) {
    // Fit the source inside the destination, keeping its aspect ratio; centre it on even offsets.
    const std::uint64_t srcByDstHeight = std::uint64_t{srcWidth} * dstHeight;
    const std::uint64_t dstBySrcHeight = std::uint64_t{dstWidth} * srcHeight;
    content_ = Rect{0, 0, dstWidth, dstHeight};
    if (srcByDstHeight > dstBySrcHeight) {  // source is wider: bars at the top and bottom
        content_.height = std::max<std::uint32_t>(2, EvenDown(dstBySrcHeight / srcWidth));
        content_.y = EvenDown((dstHeight - content_.height) / 2);
    } else if (srcByDstHeight < dstBySrcHeight) {  // source is taller: bars on the sides
        content_.width = std::max<std::uint32_t>(2, EvenDown(srcByDstHeight / srcHeight));
        content_.x = EvenDown((dstWidth - content_.width) / 2);
    }

    reducedWidth_ = srcWidth;
    reducedHeight_ = srcHeight;
    while (reducedWidth_ >= 2 * content_.width && reducedHeight_ >= 2 * content_.height &&
           reducedWidth_ % 4 == 0 && reducedHeight_ % 4 == 0) {
        reducedWidth_ /= 2;
        reducedHeight_ /= 2;
        std::vector<std::uint8_t>& buffer = scratch_[halvings_ % 2];
        if (buffer.empty()) buffer.resize(std::size_t{reducedWidth_} * reducedHeight_ * 3 / 2);
        ++halvings_;
    }

    lumaX_ = MakeTaps(reducedWidth_, content_.width);
    lumaY_ = MakeTaps(reducedHeight_, content_.height);
    chromaX_ = MakeTaps(reducedWidth_ / 2, content_.width / 2);
    chromaY_ = MakeTaps(reducedHeight_ / 2, content_.height / 2);
}

std::vector<Nv12Scaler::Tap> Nv12Scaler::MakeTaps(std::uint32_t srcSize, std::uint32_t dstSize) {
    // Pixel centres are aligned: destination sample i sits at source position
    // (i + 0.5) × src / dst − 0.5, kept in 1/256 units.
    std::vector<Tap> taps(dstSize);
    const std::uint32_t last = srcSize - 1;
    for (std::uint32_t i = 0; i < dstSize; ++i) {
        const std::int64_t position =
            static_cast<std::int64_t>((std::uint64_t{2} * i + 1) * srcSize * kWeightOne / (std::uint64_t{2} * dstSize)) -
            kWeightOne / 2;
        const std::uint64_t clamped = position < 0 ? 0 : static_cast<std::uint64_t>(position);
        const std::uint32_t first = static_cast<std::uint32_t>(clamped / kWeightOne);
        if (first >= last) {
            taps[i] = Tap{last, last, 0};
        } else {
            taps[i] = Tap{first, first + 1, static_cast<std::uint32_t>(clamped % kWeightOne)};
        }
    }
    return taps;
}

void Nv12Scaler::Scale(const Nv12Image& src, std::uint8_t* dst) {
    // Box passes: src → scratch[0] → scratch[1] → scratch[0] …
    Nv12Image current = src;
    for (std::uint32_t pass = 0; pass < halvings_; ++pass) {
        const std::uint32_t width = current.width / 2;
        const std::uint32_t height = current.height / 2;
        std::uint8_t* out = scratch_[pass % 2].data();
        const Nv12Image next = PackedNv12(out, width, height);
        HalvePlane<1>({current.y, current.yStride, current.width, current.height}, {out, width});
        HalvePlane<2>({current.uv, current.uvStride, current.width / 2, current.height / 2},
                      {out + std::size_t{width} * height, width});
        current = next;
    }

    std::uint8_t* dstY = dst;
    std::uint8_t* dstUv = dst + std::size_t{dstWidth_} * dstHeight_;
    const std::size_t stride = dstWidth_;
    const bool letterboxed = content_.width != dstWidth_ || content_.height != dstHeight_;
    if (letterboxed) {
        FillBars(dstY, stride, dstHeight_, content_.x, content_.y, content_.width, content_.height, kBlackLuma);
        FillBars(dstUv, stride, dstHeight_ / 2, content_.x, content_.y / 2, content_.width, content_.height / 2,
                 kNeutralChroma);
    }

    // The UV plane has half the rows; each chroma pixel is two bytes, so byte offsets match luma.
    const MutablePlane lumaOut{dstY + content_.y * stride + content_.x, stride};
    const MutablePlane chromaOut{dstUv + (content_.y / 2) * stride + content_.x, stride};
    const ConstPlane lumaIn{current.y, current.yStride, current.width, current.height};
    const ConstPlane chromaIn{current.uv, current.uvStride, current.width / 2, current.height / 2};

    if (current.width == content_.width && current.height == content_.height) {
        CopyPlane(lumaIn, content_.width, lumaOut);
        CopyPlane(chromaIn, content_.width, chromaOut);
        return;
    }
    BilinearPlane<1>(lumaIn, lumaOut, lumaX_, lumaY_, rows_);
    BilinearPlane<2>(chromaIn, chromaOut, chromaX_, chromaY_, rows_);
}

}  // namespace mwb::um::color
