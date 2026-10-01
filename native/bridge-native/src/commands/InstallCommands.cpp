// install / uninstall: validate the options, elevate (one UAC prompt), run the plan, report.
#include "commands/Commands.h"
#include "core/Output.h"
#include "elevation/ElevationBroker.h"
#include "install/InstallOptions.h"
#include "install/InstallService.h"
#include "platform/OsVersion.h"
#include "platform/Paths.h"
#include "platform/Process.h"

namespace mwb::native {

namespace {

// Shared flow of both commands. `execute` runs elevated and returns the outcome to report.
template <typename Execute>
ExitCode RunPrivileged(const CommandContext& context, std::wstring_view commandWord, const ParsedOptions& parsed,
                       Execute&& execute) {
    const bool elevatedChild = parsed.Has(L"elevated");
    if (!elevatedChild && !IsProcessElevated()) {
        return ElevationBroker(context.console).RunElevated(commandWord, context.args);
    }

    CommandOutcome outcome;
    try {
        if (elevatedChild && !IsProcessElevated()) {
            throw CommandError(ErrorCode::AccessDenied, "--elevated was given but the process has no administrator rights");
        }
        outcome = execute();
    } catch (...) {
        outcome = FailureOutcome(CurrentExceptionToCommandError());
    }

    if (elevatedChild) {
        ElevationBroker::SendResult(parsed.Required(L"result-pipe"), outcome.json);
    } else {
        context.console.Out().WriteLine(outcome.json);
    }
    return outcome.exit;
}

class InstallCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"install"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        const ParsedOptions parsed = InstallArgParser().Parse(context.args);
        const InstallOptions options = ToInstallOptions(parsed);
        // Fail fast on an unsupported OS before showing a UAC prompt.
        (void)ResolveCameraBackend(options.camera, QueryOsVersion());

        return RunPrivileged(context, L"install", parsed,
                             [&] { return InstallService(context.console, ModuleDirectory()).Install(options); });
    }
};

class UninstallCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"uninstall"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        const ParsedOptions parsed = UninstallArgParser().Parse(context.args);
        const UninstallOptions options = ToUninstallOptions(parsed);
        return RunPrivileged(context, L"uninstall", parsed,
                             [&] { return InstallService(context.console, ModuleDirectory()).Uninstall(options); });
    }
};

}  // namespace

std::unique_ptr<Command> MakeInstallCommand() { return std::make_unique<InstallCommand>(); }
std::unique_ptr<Command> MakeUninstallCommand() { return std::make_unique<UninstallCommand>(); }

}  // namespace mwb::native
