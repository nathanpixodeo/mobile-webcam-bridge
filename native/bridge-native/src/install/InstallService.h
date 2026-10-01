// Builds and runs the install / uninstall plans. Must run elevated.
#pragma once

#include <filesystem>
#include <string>

#include "core/Errors.h"
#include "install/InstallOptions.h"

namespace mwb::native {

class Console;

struct CommandOutcome {
    std::string json;  // the single JSON object the command prints
    ExitCode exit = ExitCode::Success;
};

// {"ok":false,"error":{...}} for a failure that happened before any step ran.
[[nodiscard]] CommandOutcome FailureOutcome(const CommandError& error);

class InstallService {
public:
    // `stageDir` is where the build output lives (the directory of bridge-native.exe).
    InstallService(Console& console, std::filesystem::path stageDir);

    [[nodiscard]] CommandOutcome Install(const InstallOptions& options);
    [[nodiscard]] CommandOutcome Uninstall(const UninstallOptions& options);

private:
    Console& console_;
    std::filesystem::path stageDir_;
};

}  // namespace mwb::native
