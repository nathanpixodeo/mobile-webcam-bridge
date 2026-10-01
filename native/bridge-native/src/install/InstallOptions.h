// Options of `install` / `uninstall` (protocol/BRIDGE_NATIVE.md §2) and the camera backend
// model shared by the installer, `status` and `doctor`.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <mwb/FrameProtocol.h>

#include "core/ArgParser.h"

namespace mwb::native {

struct OsVersion;

enum class CameraBackend { None, MediaFoundation, DirectShow };
enum class CameraChoice { Auto, MediaFoundation, DirectShow, None };

[[nodiscard]] std::string_view ToString(CameraBackend backend) noexcept;  // "none" | "mf" | "dshow"
[[nodiscard]] std::optional<CameraBackend> ParseCameraBackend(std::wstring_view text) noexcept;

struct InstallOptions {
    CameraChoice camera = CameraChoice::Auto;
    bool mic = true;
    mwb::frame::VideoMode mode{1280, 720, 30, 1};
    std::wstring friendlyName = L"Mobile Webcam";
};

struct UninstallOptions {
    bool camera = true;
    bool mic = true;

    [[nodiscard]] bool Everything() const noexcept { return camera && mic; }
};

// Option specs; the hidden --elevated / --result-pipe pair is used by the elevation broker.
[[nodiscard]] ArgParser InstallArgParser();
[[nodiscard]] ArgParser UninstallArgParser();

// Validate and convert parsed options; throw CommandError(Usage) on bad input.
[[nodiscard]] InstallOptions ToInstallOptions(const ParsedOptions& options);
[[nodiscard]] UninstallOptions ToUninstallOptions(const ParsedOptions& options);

// Resolves `auto` and rejects Media Foundation before Windows 11 (NOT_SUPPORTED_OS).
[[nodiscard]] CameraBackend ResolveCameraBackend(CameraChoice choice, const OsVersion& os);

}  // namespace mwb::native
