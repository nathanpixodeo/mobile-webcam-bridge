// The media types the DirectShow capture pin offers — every advertised camera mode
// (protocol/FRAME_PIPE.md §1) in every pixel format — and the rules for accepting a media type
// proposed by a downstream filter or by IAMStreamConfig::SetFormat.
#pragma once

#include "Framework.h"

#include <mwb/FrameProtocol.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace mwb::dshow {

enum class PixelFormat { Yuy2, Nv12, I420, Rgb24 };

// One offered media type.
struct CatalogEntry {
    frame::VideoMode mode;
    PixelFormat format;

    friend bool operator==(const CatalogEntry&, const CatalogEntry&) = default;
};

class FormatCatalog final {
public:
    // Pixel formats per mode, in IPin::EnumMediaTypes order (YUY2 is the most widely accepted
    // uncompressed camera format among DirectShow applications).
    static constexpr int kFormatCount = 4;

    // `modes` in advertising order; the first one is the default mode.
    explicit FormatCatalog(const frame::ModeList& modes) noexcept : modes_(modes) {}

    // Entries are grouped by mode: index = modeIndex × kFormatCount + formatIndex.
    [[nodiscard]] int Count() const noexcept { return static_cast<int>(modes_.size()) * kFormatCount; }
    [[nodiscard]] CatalogEntry EntryAt(int index) const noexcept;
    // Index of `entry`, or 0 (the default entry) when it is not offered.
    [[nodiscard]] int IndexOf(const CatalogEntry& entry) const noexcept;
    [[nodiscard]] const frame::VideoMode& DefaultMode() const noexcept { return modes_[0]; }

    [[nodiscard]] static REFERENCE_TIME FrameInterval(const frame::VideoMode& mode) noexcept;

    // Fills `mediaType` with entry `index` (0 ≤ index < Count()).
    [[nodiscard]] HRESULT GetMediaType(int index, CMediaType* mediaType) const;

    // The entry `mediaType` stands for if this pin can produce it: an offered size, frame rate
    // (AvgTimePerFrame; 0 = the mode's default) and subtype, a VIDEOINFOHEADER, positive height
    // (YUV top-down, RGB bottom-up), and a stride (biWidth) at least the image width — larger
    // strides are what video renderers ask for via dynamic format changes, and require rcSource
    // to describe the real image.
    [[nodiscard]] std::optional<CatalogEntry> Match(const AM_MEDIA_TYPE& mediaType) const noexcept;

    // IAMStreamConfig::GetStreamCaps payload for entry `index`.
    void FillCaps(int index, VIDEO_STREAM_CONFIG_CAPS& caps) const noexcept;

    // Bytes per row for a buffer whose stride in pixels is `strideInPixels` (= biWidth).
    [[nodiscard]] static std::size_t StrideBytes(PixelFormat format, std::uint32_t strideInPixels) noexcept;
    // Bytes of one image of `height` rows with that stride.
    [[nodiscard]] static std::size_t ImageBytes(PixelFormat format, std::uint32_t strideInPixels, std::uint32_t height) noexcept;

private:
    frame::ModeList modes_;
};

}  // namespace mwb::dshow
