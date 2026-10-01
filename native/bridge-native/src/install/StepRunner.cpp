#include "install/StepRunner.h"

#include "core/Command.h"
#include "core/Json.h"
#include "core/Output.h"

namespace mwb::native {

void RunReport::WriteSteps(JsonWriter& writer) const {
    writer.Key("steps").BeginArray();
    for (const StepReportEntry& entry : entries) {
        writer.BeginObject().Field("name", entry.name).Field("ok", entry.ok);
        if (entry.error) {
            writer.Key("error");
            WriteErrorObject(writer, *entry.error);
        }
        writer.EndObject();
    }
    writer.EndArray();
    if (rebootRequired) writer.Field("rebootRequired", true);
}

RunReport StepRunner::Run(InstallPlan& plan, RunMode mode) {
    RunReport report;
    std::vector<InstallStep*> completed;

    for (const std::unique_ptr<InstallStep>& step : plan) {
        console_.Info("step " + step->Name());
        try {
            step->Execute();
            completed.push_back(step.get());
            report.rebootRequired = report.rebootRequired || step->RebootRequired();
            report.entries.push_back(StepReportEntry{step->Name(), true, std::nullopt});
        } catch (...) {
            const CommandError error = CurrentExceptionToCommandError();
            console_.Error(step->Name() + " failed: " + error.Message());
            report.ok = false;
            if (!report.firstError) report.firstError = error;
            report.entries.push_back(StepReportEntry{step->Name(), false, error});
            if (mode == RunMode::Transactional) break;
        }
    }

    if (mode == RunMode::Transactional) {
        if (report.ok) {
            for (InstallStep* step : completed) step->Commit();
        } else {
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                const bool undone = (*it)->Rollback();
                report.entries.push_back(StepReportEntry{"rollback-" + (*it)->Name(), undone, std::nullopt});
            }
        }
    }
    return report;
}

}  // namespace mwb::native
