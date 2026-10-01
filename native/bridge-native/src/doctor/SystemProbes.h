// Read-only system queries used by the doctor checks. None of them needs elevation.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mwb::native {

enum class ServiceStartType { Missing, Disabled, Manual, Automatic, Other };

struct ServiceInfo {
    ServiceStartType startType = ServiceStartType::Missing;
    bool running = false;
};

[[nodiscard]] ServiceInfo QueryService(std::wstring_view name);

enum class ConsentValue { Unknown, Allow, Deny };

// Windows privacy setting for a capability ("webcam", "microphone"): the machine-wide value,
// the per-user value and the per-user value for desktop (non-packaged) apps.
struct PrivacyConsent {
    ConsentValue machine = ConsentValue::Unknown;
    ConsentValue user = ConsentValue::Unknown;
    ConsentValue desktopApps = ConsentValue::Unknown;

    [[nodiscard]] bool Denied() const noexcept {
        return machine == ConsentValue::Deny || user == ConsentValue::Deny || desktopApps == ConsentValue::Deny;
    }
};

[[nodiscard]] PrivacyConsent QueryPrivacyConsent(std::wstring_view capability);

[[nodiscard]] std::optional<bool> IsTestSigningEnabled();
[[nodiscard]] std::optional<bool> IsSecureBootEnabled();

// Lists \\.\pipe\ instead of connecting, because a connection to the frame pipe would count as
// a camera consumer.
[[nodiscard]] bool PipeExists(std::wstring_view name);

struct AudioEndpointInfo {
    std::wstring friendlyName;
    bool active = false;
};

// Capture endpoints (any state) whose name mentions the virtual microphone.
[[nodiscard]] std::vector<AudioEndpointInfo> FindBridgeCaptureEndpoints();

}  // namespace mwb::native
