// The media types the DirectShow capture pin offers for the installed camera mode, and the rules
// for accepting a media type proposed by a downstream filter or by IAMStreamConfig::SetFormat.
#pragma once

#include "Framework.h"

#include <mwb/FrameProtocol.h>

#include <cstddef>
#include <cstdint>

namespace mwb::dshow {

enum class PixelFormat { Yuy2, Nv12, I420, Rgb24 };

class FormatCatalog final {
public:
    // Order matters: it is the order of IPin::EnumMediaTypes (YUY2 is the most widely accepted
    // uncompressed camera format among DirectShow applications).
    static constexpr int kCount = 4;

    explicit FormatCatalog(const frame::VideoMode& mode) noexcept : mode_(mode) {}

    [[nodiscard]] const frame::VideoMode& Mode() const noexcept { return mode_; }
    [[nodiscard]] REFERENCE_TIME FrameInterval() const noexcept;

    [[nodiscard]] static PixelFormat FormatAt(int index) noexcept;
    [[nodiscard]] static int IndexOf(PixelFormat format) noexcept;

    // Fills `mediaType` with catalog entry `index` (0 ≤ index < kCount).
    [[nodiscard]] HRESULT GetMediaType(int index, CMediaType* mediaType) const;

    // The pixel format of `mediaType` if this pin can produce it: our size and subtype, a
    // VIDEOINFOHEADER, positive height (YUV top-down, RGB bottom-up), and a stride (biWidth) at
    // least the image width — larger strides are what video renderers ask for via dynamic format
    // changes, and require rcSource to describe the real image.
    [[nodiscard]] std::optional<PixelFormat> Match(const AM_MEDIA_TYPE& mediaType) const noexcept;

    // IAMStreamConfig::GetStreamCaps payload for entry `index`.
    void FillCaps(int index, VIDEO_STREAM_CONFIG_CAPS& caps) const noexcept;

    // Bytes per row for a buffer whose stride in pixels is `strideInPixels` (= biWidth).
    [[nodiscard]] static std::size_t StrideBytes(PixelFormat format, std::uint32_t strideInPixels) noexcept;
    // Bytes of one image of `height` rows with that stride.
    [[nodiscard]] static std::size_t ImageBytes(PixelFormat format, std::uint32_t strideInPixels, std::uint32_t height) noexcept;

private:
    frame::VideoMode mode_;
};

}  // namespace mwb::dshow
