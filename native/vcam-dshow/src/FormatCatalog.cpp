#include "Framework.h"

#include "FormatCatalog.h"

#include <mwb/um/ColorConvert.h>

#include <algorithm>
#include <climits>

namespace mwb::dshow {

namespace {

struct FormatSpec {
    PixelFormat format;
    DWORD fourcc;      // biCompression (BI_RGB for RGB24)
    WORD bitsPerPixel;
};

constexpr FormatSpec kSpecs[FormatCatalog::kFormatCount] = {
    {PixelFormat::Yuy2, MAKEFOURCC('Y', 'U', 'Y', '2'), 16},
    {PixelFormat::Nv12, MAKEFOURCC('N', 'V', '1', '2'), 12},
    {PixelFormat::I420, MAKEFOURCC('I', '4', '2', '0'), 12},
    {PixelFormat::Rgb24, BI_RGB, 24},
};

GUID SubtypeOf(const FormatSpec& spec) {
    // YUV subtypes are FOURCC-derived GUIDs; RGB24 has its own.
    return spec.format == PixelFormat::Rgb24 ? MEDIASUBTYPE_RGB24 : static_cast<GUID>(FOURCCMap(spec.fourcc));
}

constexpr LONG kMaxStridePadding = 4096;

// Bits per second of raw video; 64-bit (4K RGB24 at 30 fps is ~6 Gbit/s), clamped by the callers.
long long BitsPerSecond(std::size_t imageBytes, const frame::VideoMode& mode) noexcept {
    return static_cast<long long>(imageBytes) * 8 * mode.fpsNum / mode.fpsDen;
}

}  // namespace

REFERENCE_TIME FormatCatalog::FrameInterval(const frame::VideoMode& mode) noexcept {
    return static_cast<REFERENCE_TIME>(10'000'000ull * mode.fpsDen / mode.fpsNum);
}

CatalogEntry FormatCatalog::EntryAt(int index) const noexcept {
    return CatalogEntry{modes_[static_cast<std::size_t>(index / kFormatCount)], kSpecs[index % kFormatCount].format};
}

int FormatCatalog::IndexOf(const CatalogEntry& entry) const noexcept {
    for (int i = 0; i < Count(); ++i) {
        if (EntryAt(i) == entry) return i;
    }
    return 0;
}

std::size_t FormatCatalog::StrideBytes(PixelFormat format, std::uint32_t strideInPixels) noexcept {
    switch (format) {
        case PixelFormat::Yuy2: return static_cast<std::size_t>(strideInPixels) * 2;
        case PixelFormat::Nv12:
        case PixelFormat::I420: return strideInPixels;
        case PixelFormat::Rgb24: return um::color::Rgb24Pitch(strideInPixels);
    }
    return 0;
}

std::size_t FormatCatalog::ImageBytes(PixelFormat format, std::uint32_t strideInPixels, std::uint32_t height) noexcept {
    const std::size_t rows = (format == PixelFormat::Nv12 || format == PixelFormat::I420)
                                 ? static_cast<std::size_t>(height) * 3 / 2
                                 : height;
    return StrideBytes(format, strideInPixels) * rows;
}

HRESULT FormatCatalog::GetMediaType(int index, CMediaType* mediaType) const {
    CheckPointer(mediaType, E_POINTER);
    if (index < 0 || index >= Count()) return E_INVALIDARG;
    const FormatSpec& spec = kSpecs[index % kFormatCount];
    const frame::VideoMode mode = EntryAt(index).mode;

    auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mediaType->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    if (info == nullptr) return E_OUTOFMEMORY;
    ZeroMemory(info, sizeof(VIDEOINFOHEADER));

    const auto imageBytes = static_cast<DWORD>(ImageBytes(spec.format, mode.width, mode.height));
    info->AvgTimePerFrame = FrameInterval(mode);
    info->dwBitRate = static_cast<DWORD>(std::min<long long>(BitsPerSecond(imageBytes, mode), MAXDWORD));
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = static_cast<LONG>(mode.width);
    info->bmiHeader.biHeight = static_cast<LONG>(mode.height);  // YUV: top-down; RGB: bottom-up
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = spec.bitsPerPixel;
    info->bmiHeader.biCompression = spec.fourcc;
    info->bmiHeader.biSizeImage = imageBytes;

    const GUID subtype = SubtypeOf(spec);
    mediaType->SetType(&MEDIATYPE_Video);
    mediaType->SetSubtype(&subtype);
    mediaType->SetFormatType(&FORMAT_VideoInfo);
    mediaType->SetTemporalCompression(FALSE);
    mediaType->SetSampleSize(imageBytes);
    return S_OK;
}

std::optional<CatalogEntry> FormatCatalog::Match(const AM_MEDIA_TYPE& mediaType) const noexcept {
    if (mediaType.majortype != MEDIATYPE_Video || mediaType.formattype != FORMAT_VideoInfo ||
        mediaType.pbFormat == nullptr || mediaType.cbFormat < sizeof(VIDEOINFOHEADER)) {
        return std::nullopt;
    }

    const FormatSpec* spec = nullptr;
    for (const FormatSpec& candidate : kSpecs) {
        if (SubtypeOf(candidate) == mediaType.subtype) spec = &candidate;
    }
    if (spec == nullptr) return std::nullopt;

    const auto& info = *reinterpret_cast<const VIDEOINFOHEADER*>(mediaType.pbFormat);
    const BITMAPINFOHEADER& bitmap = info.bmiHeader;
    if (bitmap.biCompression != spec->fourcc || bitmap.biBitCount != spec->bitsPerPixel) return std::nullopt;
    if (bitmap.biWidth <= 0 || bitmap.biHeight <= 0) return std::nullopt;

    // The image width is rcSource's when set (biWidth is then the stride), else biWidth.
    const RECT& source = info.rcSource;
    const bool hasSource = source.right > source.left;
    if (hasSource && (source.left != 0 || source.top != 0 || source.bottom != bitmap.biHeight)) return std::nullopt;
    const LONG width = hasSource ? source.right : bitmap.biWidth;
    const LONG height = bitmap.biHeight;
    if (bitmap.biWidth < width || bitmap.biWidth > width + kMaxStridePadding) return std::nullopt;
    // A padded stride is only meaningful when rcSource names the real image.
    if (bitmap.biWidth != width && !hasSource) return std::nullopt;

    const std::optional<frame::VideoMode> mode = frame::FindModeByDuration(
        modes_, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), info.AvgTimePerFrame);
    if (!mode) return std::nullopt;

    const std::size_t required = ImageBytes(spec->format, static_cast<std::uint32_t>(bitmap.biWidth), mode->height);
    if (bitmap.biSizeImage != 0 && bitmap.biSizeImage < required) return std::nullopt;
    return CatalogEntry{*mode, spec->format};
}

void FormatCatalog::FillCaps(int index, VIDEO_STREAM_CONFIG_CAPS& caps) const noexcept {
    ZeroMemory(&caps, sizeof(caps));
    const CatalogEntry entry = EntryAt(index);
    const frame::VideoMode& mode = entry.mode;
    const SIZE size{static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
    const std::size_t imageBytes = ImageBytes(entry.format, mode.width, mode.height);

    caps.guid = FORMAT_VideoInfo;
    caps.InputSize = size;
    caps.MinCroppingSize = size;
    caps.MaxCroppingSize = size;
    caps.CropGranularityX = 1;
    caps.CropGranularityY = 1;
    caps.CropAlignX = 1;
    caps.CropAlignY = 1;
    caps.MinOutputSize = size;
    caps.MaxOutputSize = size;
    caps.OutputGranularityX = 1;
    caps.OutputGranularityY = 1;
    caps.MinFrameInterval = FrameInterval(mode);
    caps.MaxFrameInterval = FrameInterval(mode);
    // RGB24 1080p60 exceeds LONG_MAX bits per second: clamp rather than overflow.
    caps.MinBitsPerSecond = static_cast<LONG>(std::min<long long>(BitsPerSecond(imageBytes, mode), LONG_MAX));
    caps.MaxBitsPerSecond = caps.MinBitsPerSecond;
}

}  // namespace mwb::dshow
