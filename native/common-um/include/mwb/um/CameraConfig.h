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
    frame::VideoMode mode{1280, 720, 30, 1};
    std::wstring pipeName{frame::kDefaultPublicPipeName};
    std::wstring friendlyName{L"Mobile Webcam"};
};

// Fills `settings` from the registry. Returns S_OK when the key exists, S_FALSE when it does not
// (defaults are used), or a failure HRESULT on allocation/registry errors (defaults are used).
[[nodiscard]] HRESULT LoadCameraSettings(CameraSettings& settings) noexcept;

// Pipe names are restricted to [A-Za-z0-9._-]{1,128}; the registry is not trusted blindly.
[[nodiscard]] bool IsValidPipeName(std::wstring_view name) noexcept;

// "\\.\pipe\" + name.
[[nodiscard]] std::wstring PipePath(std::wstring_view name);

}  // namespace mwb::um
