// One unit of install/uninstall work, reported as one entry of the "steps" array.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mwb::native {

class JsonWriter;

class InstallStep {
public:
    virtual ~InstallStep() = default;

    [[nodiscard]] virtual std::string Name() const = 0;

    // Performs the step; throws CommandError on failure.
    virtual void Execute() = 0;

    // Undoes a successfully executed step (install runs only). Returns false if it could not.
    virtual bool Rollback() noexcept { return true; }

    // Called once every step succeeded, to drop backups kept for Rollback.
    virtual void Commit() noexcept {}

    // Whether Execute left something that only a reboot completes.
    [[nodiscard]] virtual bool RebootRequired() const noexcept { return false; }
};

using InstallPlan = std::vector<std::unique_ptr<InstallStep>>;

}  // namespace mwb::native
