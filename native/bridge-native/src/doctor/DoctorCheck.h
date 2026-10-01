// `bridge-native doctor`: independent checks, each reporting pass / warn / fail with a message.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "install/MicDriver.h"
#include "install/ProductRegistry.h"
#include "platform/OsVersion.h"

namespace mwb::native {

enum class CheckStatus { Pass, Warn, Fail };

[[nodiscard]] std::string_view ToString(CheckStatus status) noexcept;

struct CheckResult {
    CheckStatus status = CheckStatus::Pass;
    std::string message;
};

// Facts gathered once and shared by all checks.
struct DoctorContext {
    OsVersion os;
    std::optional<ProductRegistration> product;
    std::optional<CameraRegistration> camera;
    MicDeviceState mic;
};

class DoctorCheck {
public:
    virtual ~DoctorCheck() = default;
    [[nodiscard]] virtual std::string_view Id() const = 0;
    [[nodiscard]] virtual CheckResult Run(const DoctorContext& context) const = 0;
};

// All checks listed in protocol/BRIDGE_NATIVE.md, in report order.
[[nodiscard]] std::vector<std::unique_ptr<DoctorCheck>> CreateDoctorChecks();

}  // namespace mwb::native
