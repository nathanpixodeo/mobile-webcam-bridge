// Camera settings shared by the camera consumers, read from
// HKLM\SOFTWARE\MobileWebcamBridge\Camera (written by `bridge-native install`).
//
// The consumers run inside Frame Server and inside arbitrary third-party processes, so every value
// is validated before it can size a buffer or name a pipe; anything unexpected falls back to the
// defaults below.
#pragma once

#include <mwb/FrameProtocol.h>

#include <windows.h>

#include <string>
#include <string_view>

namespace mwb::um {

struct CameraSettings {
    // Advertised first (registry Width, Height, FpsNum, FpsDen); always an advertised mode.
    frame::VideoMode defaultMode{1920, 1080, 30, 1};
    // Limit on the advertised modes (registry MaxWidth, MaxHeight, MaxFps). Installations from
    // before the cap existed have none; they advertise their single mode, as they did then.
    frame::ModeCap cap{frame::kFullCatalogCap};
    std::wstring pipeName{frame::kDefaultPublicPipeName};
    std::wstring friendlyName{L"Mobile Webcam"};

    // The modes the camera offers, in advertising order (protocol/FRAME_PIPE.md §1).
    [[nodiscard]] frame::ModeList AdvertisedModes() const noexcept { return frame::AdvertisedModes(cap, defaultMode); }
};

// Fills `settings` from the registry. Returns S_OK when the key exists, S_FALSE when it does not
// (defaults are used), or a failure HRESULT on allocation/registry errors (defaults are used).
[[nodiscard]] HRESULT LoadCameraSettings(CameraSettings& settings) noexcept;

// Pipe names are restricted to [A-Za-z0-9._-]{1,128}; the registry is not trusted blindly.
[[nodiscard]] bool IsValidPipeName(std::wstring_view name) noexcept;

// "\\.\pipe\" + name.
[[nodiscard]] std::wstring PipePath(std::wstring_view name);

}  // namespace mwb::um
