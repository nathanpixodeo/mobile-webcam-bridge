// Exclusive handle to the virtual microphone's control device (\\.\MobileWebcamBridgeMic) and its
// IOCTLs (protocol/MIC_FEED.md).
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>

#include <mwb/MwbMicIoctl.h>
#include <wil/resource.h>

namespace mwb::native {

struct MicStatus {
    std::uint32_t bufferedBytes = 0;
    std::uint32_t capacityBytes = 0;
    bool streamActive = false;
    std::uint32_t underruns = 0;
    std::uint32_t overruns = 0;
};

enum class MicOpenError { None, NotPresent, Busy, AccessDenied, Other };

class MicDevice {
public:
    // nullopt with `error` set when the device cannot be opened.
    [[nodiscard]] static std::optional<MicDevice> Open(MicOpenError& error, DWORD& win32Error);

    // Validates the driver ABI; throws CommandError(Internal) on mismatch.
    [[nodiscard]] MWBMIC_VERSION QueryVersion() const;
    [[nodiscard]] MicStatus Write(std::span<const std::uint8_t> pcm) const;
    [[nodiscard]] MicStatus QueryStatus() const;

private:
    explicit MicDevice(wil::unique_handle handle) noexcept : handle_(std::move(handle)) {}

    wil::unique_handle handle_;
};

}  // namespace mwb::native
