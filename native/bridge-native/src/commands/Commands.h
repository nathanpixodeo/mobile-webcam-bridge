// Factories for every bridge-native command (protocol/BRIDGE_NATIVE.md).
#pragma once

#include <memory>

#include "core/Command.h"

namespace mwb::native {

[[nodiscard]] std::unique_ptr<Command> MakeVersionCommand();
[[nodiscard]] std::unique_ptr<Command> MakeStatusCommand();
[[nodiscard]] std::unique_ptr<Command> MakeInstallCommand();
[[nodiscard]] std::unique_ptr<Command> MakeUninstallCommand();
[[nodiscard]] std::unique_ptr<Command> MakeDoctorCommand();
[[nodiscard]] std::unique_ptr<Command> MakeVideoHubCommand();
[[nodiscard]] std::unique_ptr<Command> MakeVideoWatchCommand();
[[nodiscard]] std::unique_ptr<Command> MakeMicFeedCommand();
[[nodiscard]] std::unique_ptr<Command> MakeMicStatusCommand();

}  // namespace mwb::native
