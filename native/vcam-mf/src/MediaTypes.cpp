#include "Framework.h"

#include "MediaTypes.h"

#include <algorithm>

namespace mwb::vcam {

namespace {

struct FormatTraits {
    GUID subtype;
    UINT32 bitsPerPixel;
    UINT32 strideMultiplier;  // default stride = width × multiplier (bytes per luma sample)
};

FormatTraits TraitsOf(OutputFormat format) noexcept {
    // NV12: the default stride is the luma row (width), not width × 1.5 — a classic mistake that
    // makes some consumers compute a wrong chroma offset.
    return format == OutputFormat::Nv12 ? FormatTraits{MFVideoFormat_NV12, 12, 1} : FormatTraits{MFVideoFormat_YUY2, 16, 2};
}

}  // namespace

HRESULT CreateMediaType(const frame::VideoMode& mode, OutputFormat format, IMFMediaType** mediaType) noexcept {
    RETURN_HR_IF_NULL(E_POINTER, mediaType);
    *mediaType = nullptr;

    const FormatTraits traits = TraitsOf(format);
    const UINT32 sampleSize = mode.width * mode.height * traits.bitsPerPixel / 8;
    // 64-bit: raw 4K YUY2 at 30 fps is ~4 Gbit/s, more than a UINT32 holds at 60 fps.
    const UINT64 bitrate = UINT64{sampleSize} * 8 * mode.fpsNum / mode.fpsDen;

    wil::com_ptr_nothrow<IMFMediaType> type;
    RETURN_IF_FAILED(MFCreateMediaType(&type));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, traits.subtype));
    RETURN_IF_FAILED(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, mode.width, mode.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, mode.fpsNum, mode.fpsDen));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, mode.width * traits.strideMultiplier));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::min<UINT64>(bitrate, UINT32_MAX))));

    // BT.709, limited range — what the hub delivers (protocol/FRAME_PIPE.md).
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));

    *mediaType = type.detach();
    return S_OK;
}

HRESULT CreateMediaTypes(const frame::ModeList& modes, std::vector<wil::com_ptr_nothrow<IMFMediaType>>& types) noexcept try {
    types.clear();
    types.reserve(modes.size() * 2);
    for (const frame::VideoMode& mode : modes) {
        for (const OutputFormat format : {OutputFormat::Nv12, OutputFormat::Yuy2}) {
            wil::com_ptr_nothrow<IMFMediaType> type;
            RETURN_IF_FAILED(CreateMediaType(mode, format, type.put()));
            types.push_back(std::move(type));
        }
    }
    return S_OK;
}
CATCH_RETURN()

HRESULT ResolveMediaType(IMFMediaType* mediaType, const frame::ModeList& modes, ResolvedType* resolved) noexcept {
    RETURN_HR_IF_NULL(E_POINTER, mediaType);
    RETURN_HR_IF_NULL(E_POINTER, resolved);

    GUID majorType{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    RETURN_IF_FAILED(mediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
    RETURN_IF_FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype));
    RETURN_IF_FAILED(MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height));
    UINT32 fpsNum = 0;
    UINT32 fpsDen = 0;
    if (FAILED(MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &fpsNum, &fpsDen))) {
        fpsNum = fpsDen = 0;  // unspecified: the first advertised mode of that size
    }

    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, majorType != MFMediaType_Video);
    const std::optional<frame::VideoMode> mode = frame::FindMode(modes, width, height, fpsNum, fpsDen);
    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, !mode.has_value());
    if (subtype == MFVideoFormat_NV12) {
        *resolved = ResolvedType{*mode, OutputFormat::Nv12};
    } else if (subtype == MFVideoFormat_YUY2) {
        *resolved = ResolvedType{*mode, OutputFormat::Yuy2};
    } else {
        return MF_E_INVALIDMEDIATYPE;
    }
    return S_OK;
}

}  // namespace mwb::vcam
