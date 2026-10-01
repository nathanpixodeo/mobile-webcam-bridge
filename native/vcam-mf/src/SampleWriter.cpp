#include "Framework.h"

#include "SampleWriter.h"

#include <mwb/um/ColorConvert.h>

namespace mwb::vcam {

std::size_t SampleWriter::RowBytes() const noexcept {
    return format_ == OutputFormat::Nv12 ? mode_.width : static_cast<std::size_t>(mode_.width) * 2;
}

std::size_t SampleWriter::ImageRows() const noexcept {
    // NV12 = luma rows + half as many chroma rows, all sharing the same pitch.
    return format_ == OutputFormat::Nv12 ? static_cast<std::size_t>(mode_.height) * 3 / 2 : mode_.height;
}

void SampleWriter::Convert(const std::uint8_t* nv12, std::uint8_t* destination, std::size_t pitch) const noexcept {
    const um::color::Nv12Image source = um::color::PackedNv12(nv12, mode_.width, mode_.height);
    if (format_ == OutputFormat::Nv12) {
        um::color::CopyNv12(source, destination, pitch);
    } else {
        um::color::Nv12ToYuy2(source, destination, pitch);
    }
}

HRESULT SampleWriter::Write(IMFSample* sample, const std::uint8_t* nv12) const noexcept {
    RETURN_HR_IF_NULL(E_POINTER, sample);
    RETURN_HR_IF_NULL(E_POINTER, nv12);

    wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    if (auto buffer2D = buffer.try_query<IMF2DBuffer2>()) {
        RETURN_IF_FAILED(WriteWith2DBuffer(buffer2D.get(), nv12));
        // Some consumers read GetCurrentLength() even for 2D buffers.
        DWORD contiguousLength = 0;
        if (SUCCEEDED(buffer2D->GetContiguousLength(&contiguousLength))) {
            LOG_IF_FAILED(buffer->SetCurrentLength(contiguousLength));
        }
        return S_OK;
    }
    return WriteWithLinearBuffer(buffer.get(), nv12);
}

HRESULT SampleWriter::WriteWith2DBuffer(IMF2DBuffer2* buffer, const std::uint8_t* nv12) const noexcept {
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    BYTE* bufferStart = nullptr;
    DWORD bufferLength = 0;
    RETURN_IF_FAILED(buffer->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &bufferStart, &bufferLength));
    auto unlock = wil::scope_exit([&]() noexcept { buffer->Unlock2D(); });

    // YUV layouts are always top-down; a negative pitch or a buffer too small for the image means
    // the allocator gave us something we did not negotiate, so refuse instead of overrunning it.
    RETURN_HR_IF(MF_E_UNSUPPORTED_FORMAT, pitch <= 0 || static_cast<std::size_t>(pitch) < RowBytes());
    const std::size_t offset = static_cast<std::size_t>(scanline0 - bufferStart);
    const std::size_t required = static_cast<std::size_t>(pitch) * ImageRows();
    RETURN_HR_IF(MF_E_BUFFERTOOSMALL, scanline0 < bufferStart || offset + required > bufferLength);

    Convert(nv12, scanline0, static_cast<std::size_t>(pitch));
    return S_OK;
}

HRESULT SampleWriter::WriteWithLinearBuffer(IMFMediaBuffer* buffer, const std::uint8_t* nv12) const noexcept {
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
    auto unlock = wil::scope_exit([&]() noexcept { buffer->Unlock(); });

    const std::size_t required = RowBytes() * ImageRows();
    RETURN_HR_IF(MF_E_BUFFERTOOSMALL, required > maxLength);

    Convert(nv12, data, RowBytes());
    unlock.reset();
    RETURN_IF_FAILED(buffer->SetCurrentLength(static_cast<DWORD>(required)));
    return S_OK;
}

}  // namespace mwb::vcam
