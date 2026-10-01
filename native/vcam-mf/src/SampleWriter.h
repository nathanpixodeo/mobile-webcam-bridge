// Writes one tightly packed NV12 frame into an IMFSample allocated by the pipeline's allocator,
// honouring whatever pitch the buffer has (2D buffers may be padded or GPU backed).
#pragma once

#include "Framework.h"
#include "MediaTypes.h"

#include <mwb/FrameProtocol.h>

#include <cstdint>

namespace mwb::vcam {

class SampleWriter final {
public:
    SampleWriter(const frame::VideoMode& mode, OutputFormat format) noexcept : mode_(mode), format_(format) {}

    [[nodiscard]] HRESULT Write(IMFSample* sample, const std::uint8_t* nv12) const noexcept;

private:
    [[nodiscard]] std::size_t RowBytes() const noexcept;
    [[nodiscard]] std::size_t ImageRows() const noexcept;  // rows of `pitch` bytes the image needs
    [[nodiscard]] HRESULT WriteWith2DBuffer(IMF2DBuffer2* buffer, const std::uint8_t* nv12) const noexcept;
    [[nodiscard]] HRESULT WriteWithLinearBuffer(IMFMediaBuffer* buffer, const std::uint8_t* nv12) const noexcept;
    void Convert(const std::uint8_t* nv12, std::uint8_t* destination, std::size_t pitch) const noexcept;

    frame::VideoMode mode_;
    OutputFormat format_;
};

}  // namespace mwb::vcam
