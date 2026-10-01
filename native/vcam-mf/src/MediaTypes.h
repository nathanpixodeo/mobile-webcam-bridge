// Media types offered by the virtual camera stream: the installed mode in NV12 (preferred) and
// YUY2 (for older consumers reached through the Frame Server DirectShow bridge).
#pragma once

#include "Framework.h"

#include <mwb/FrameProtocol.h>

#include <array>

namespace mwb::vcam {

enum class OutputFormat { Nv12, Yuy2 };

inline constexpr std::size_t kMediaTypeCount = 2;

[[nodiscard]] HRESULT CreateMediaType(const frame::VideoMode& mode, OutputFormat format, IMFMediaType** mediaType) noexcept;

// All offered types, preferred first.
[[nodiscard]] HRESULT CreateMediaTypes(const frame::VideoMode& mode,
                                       std::array<wil::com_ptr_nothrow<IMFMediaType>, kMediaTypeCount>& types) noexcept;

// Maps a type selected by the pipeline back to an output format; fails with MF_E_INVALIDMEDIATYPE
// for anything this stream did not offer.
[[nodiscard]] HRESULT ResolveOutputFormat(IMFMediaType* mediaType, const frame::VideoMode& mode, OutputFormat* format) noexcept;

// Frame duration in 100 ns units.
[[nodiscard]] constexpr LONGLONG FrameDuration(const frame::VideoMode& mode) noexcept {
    return mode.fpsNum == 0 ? 333'333 : static_cast<LONGLONG>(10'000'000ull * mode.fpsDen / mode.fpsNum);
}

}  // namespace mwb::vcam
