// Root-enumerated device node and driver package of mwbmic.sys (hardware id
// ROOT\MobileWebcamBridgeMic). This is the "devcon install / remove" logic, done with SetupAPI.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mwb::native {

struct MicDeviceState {
    bool installed = false;           // a device node with our hardware id exists
    bool present = false;             // ... and is currently present (started or with a problem)
    unsigned long problemCode = 0;    // CM_PROB_* of the present node, 0 when none
};

struct DriverChange {
    bool createdDeviceNode = false;
    bool rebootRequired = false;
};

class MicDriver {
public:
    [[nodiscard]] MicDeviceState QueryState() const;

    // Creates the device node when missing, then installs/updates the driver from `infPath`.
    // When the driver install fails, a node created by this call is removed again.
    [[nodiscard]] DriverChange Install(const std::filesystem::path& infPath) const;

    // Removes every device node with our hardware id and the driver packages they used.
    [[nodiscard]] DriverChange Uninstall() const;
};

}  // namespace mwb::native
