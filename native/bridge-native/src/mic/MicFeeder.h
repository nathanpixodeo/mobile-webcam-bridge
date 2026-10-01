// `bridge-native mic feed`: stdin PCM → 10 ms chunks → IOCTL_MWBMIC_WRITE, plus a status event
// every 100 ms. Exits when stdin closes; closing the device resets the driver's ring.
#pragma once

#include <mutex>
#include <optional>

#include "core/Errors.h"
#include "mic/MicDevice.h"

namespace mwb::native {

class Console;

// Maps a failed open to the contract's error codes (DEVICE_NOT_PRESENT → exit 3, DEVICE_BUSY).
[[nodiscard]] CommandError MicOpenFailure(MicOpenError error, unsigned long win32Error);

class MicFeeder {
public:
    explicit MicFeeder(Console& console) noexcept : console_(console) {}

    [[nodiscard]] ExitCode Run();

private:
    void EmitReady(const MWBMIC_VERSION& version);
    void EmitStatus(const MicStatus& status);

    Console& console_;
};

}  // namespace mwb::native
