// Runs an InstallPlan and produces the report described in protocol/BRIDGE_NATIVE.md §2.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "install/InstallStep.h"

namespace mwb::native {

class Console;
class JsonWriter;

enum class RunMode {
    Transactional,  // install: stop at the first failure and roll back completed steps
    BestEffort,     // uninstall: run every step, collect failures
};

struct StepReportEntry {
    std::string name;
    bool ok = true;
    std::optional<CommandError> error;
};

struct RunReport {
    bool ok = true;
    bool rebootRequired = false;
    std::vector<StepReportEntry> entries;
    std::optional<CommandError> firstError;

    // Writes "steps": [...] (and "rebootRequired": true when set) into the open object.
    void WriteSteps(JsonWriter& writer) const;
};

class StepRunner {
public:
    explicit StepRunner(Console& console) noexcept : console_(console) {}

    [[nodiscard]] RunReport Run(InstallPlan& plan, RunMode mode);

private:
    Console& console_;
};

}  // namespace mwb::native
