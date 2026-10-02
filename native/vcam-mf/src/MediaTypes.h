// Media types offered by the virtual camera stream: every advertised mode (protocol/FRAME_PIPE.md
// §1) in NV12 (preferred) and YUY2 (for older consumers reached through the Frame Server
// DirectShow bridge). Applications pick one; the stream then subscribes to that mode.
#pragma once

#include "Framework.h"

#include <mwb/FrameProtocol.h>

#include <vector>

namespace mwb::vcam {

enum class OutputFormat { Nv12, Yuy2 };

// What a selected media type stands for.
struct ResolvedType {
    frame::VideoMode mode;
    OutputFormat format;
};

[[nodiscard]] HRESULT CreateMediaType(const frame::VideoMode& mode, OutputFormat format, IMFMediaType** mediaType) noexcept;

// All offered types in advertising order, NV12 before YUY2 for each mode.
[[nodiscard]] HRESULT CreateMediaTypes(const frame::ModeList& modes,
                                       std::vector<wil::com_ptr_nothrow<IMFMediaType>>& types) noexcept;

// Maps a type selected by the pipeline back to an advertised mode and output format; fails with
// MF_E_INVALIDMEDIATYPE for anything this stream did not offer.
[[nodiscard]] HRESULT ResolveMediaType(IMFMediaType* mediaType, const frame::ModeList& modes, ResolvedType* resolved) noexcept;

// Frame duration in 100 ns units.
[[nodiscard]] constexpr LONGLONG FrameDuration(const frame::VideoMode& mode) noexcept {
    return mode.fpsNum == 0 ? 333'333 : static_cast<LONGLONG>(10'000'000ull * mode.fpsDen / mode.fpsNum);
}

}  // namespace mwb::vcam
