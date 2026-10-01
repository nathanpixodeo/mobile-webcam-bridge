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

constexpr FormatSpec kSpecs[FormatCatalog::kCount] = {
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

}  // namespace

REFERENCE_TIME FormatCatalog::FrameInterval() const noexcept {
    return static_cast<REFERENCE_TIME>(10'000'000ull * mode_.fpsDen / mode_.fpsNum);
}

PixelFormat FormatCatalog::FormatAt(int index) noexcept {
    return kSpecs[index].format;
}

int FormatCatalog::IndexOf(PixelFormat format) noexcept {
    for (int i = 0; i < kCount; ++i) {
        if (kSpecs[i].format == format) return i;
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
    if (index < 0 || index >= kCount) return E_INVALIDARG;
    const FormatSpec& spec = kSpecs[index];

    auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mediaType->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    if (info == nullptr) return E_OUTOFMEMORY;
    ZeroMemory(info, sizeof(VIDEOINFOHEADER));

    const auto imageBytes = static_cast<DWORD>(ImageBytes(spec.format, mode_.width, mode_.height));
    info->AvgTimePerFrame = FrameInterval();
    info->dwBitRate = imageBytes * 8 * mode_.fpsNum / mode_.fpsDen;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = static_cast<LONG>(mode_.width);
    info->bmiHeader.biHeight = static_cast<LONG>(mode_.height);  // YUV: top-down; RGB: bottom-up
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

std::optional<PixelFormat> FormatCatalog::Match(const AM_MEDIA_TYPE& mediaType) const noexcept {
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
    const auto width = static_cast<LONG>(mode_.width);
    const auto height = static_cast<LONG>(mode_.height);

    if (bitmap.biCompression != spec->fourcc || bitmap.biBitCount != spec->bitsPerPixel) return std::nullopt;
    if (bitmap.biHeight != height) return std::nullopt;
    if (bitmap.biWidth < width || bitmap.biWidth > width + kMaxStridePadding) return std::nullopt;

    if (bitmap.biWidth != width) {
        // A padded stride is only meaningful when rcSource names the real image.
        const RECT& source = info.rcSource;
        if (source.left != 0 || source.top != 0 || source.right != width || source.bottom != height) return std::nullopt;
    }

    const std::size_t required = ImageBytes(spec->format, static_cast<std::uint32_t>(bitmap.biWidth), mode_.height);
    if (bitmap.biSizeImage != 0 && bitmap.biSizeImage < required) return std::nullopt;
    return spec->format;
}

void FormatCatalog::FillCaps(int index, VIDEO_STREAM_CONFIG_CAPS& caps) const noexcept {
    ZeroMemory(&caps, sizeof(caps));
    const SIZE size{static_cast<LONG>(mode_.width), static_cast<LONG>(mode_.height)};
    const std::size_t imageBytes = ImageBytes(FormatAt(index), mode_.width, mode_.height);

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
    caps.MinFrameInterval = FrameInterval();
    caps.MaxFrameInterval = FrameInterval();
    // RGB24 1080p60 exceeds LONG_MAX bits per second: clamp rather than overflow.
    const long long bitsPerSecond = static_cast<long long>(imageBytes) * 8 * mode_.fpsNum / mode_.fpsDen;
    caps.MinBitsPerSecond = static_cast<LONG>(std::min<long long>(bitsPerSecond, LONG_MAX));
    caps.MaxBitsPerSecond = caps.MinBitsPerSecond;
}

}  // namespace mwb::dshow
