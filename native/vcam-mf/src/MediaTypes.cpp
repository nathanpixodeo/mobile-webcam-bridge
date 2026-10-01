#include "Framework.h"

#include "MediaTypes.h"

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
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, sampleSize * 8 * mode.fpsNum / mode.fpsDen));

    // BT.709, limited range — what the hub delivers (protocol/FRAME_PIPE.md).
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));

    *mediaType = type.detach();
    return S_OK;
}

HRESULT CreateMediaTypes(const frame::VideoMode& mode,
                         std::array<wil::com_ptr_nothrow<IMFMediaType>, kMediaTypeCount>& types) noexcept {
    RETURN_IF_FAILED(CreateMediaType(mode, OutputFormat::Nv12, types[0].put()));
    RETURN_IF_FAILED(CreateMediaType(mode, OutputFormat::Yuy2, types[1].put()));
    return S_OK;
}

HRESULT ResolveOutputFormat(IMFMediaType* mediaType, const frame::VideoMode& mode, OutputFormat* format) noexcept {
    RETURN_HR_IF_NULL(E_POINTER, mediaType);
    RETURN_HR_IF_NULL(E_POINTER, format);

    GUID majorType{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    RETURN_IF_FAILED(mediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
    RETURN_IF_FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype));
    RETURN_IF_FAILED(MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height));

    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, majorType != MFMediaType_Video);
    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, width != mode.width || height != mode.height);
    if (subtype == MFVideoFormat_NV12) {
        *format = OutputFormat::Nv12;
    } else if (subtype == MFVideoFormat_YUY2) {
        *format = OutputFormat::Yuy2;
    } else {
        return MF_E_INVALIDMEDIATYPE;
    }
    return S_OK;
}

}  // namespace mwb::vcam
