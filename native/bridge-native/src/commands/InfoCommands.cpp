// version, status, doctor: read-only, one-shot commands.
#include <mwb/Identifiers.h>

#include "commands/Commands.h"
#include "core/ArgParser.h"
#include "core/Json.h"
#include "core/Output.h"
#include "doctor/DoctorCheck.h"
#include "install/MicDriver.h"
#include "install/ProductRegistry.h"
#include "platform/OsVersion.h"

namespace mwb::native {

namespace {

void RejectArguments(const CommandContext& context) { (void)ArgParser{}.Parse(context.args); }

class VersionCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"version"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        RejectArguments(context);
        JsonWriter writer;
        writer.BeginObject()
            .Field("ok", true)
            .Field("version", std::wstring_view(mwb::ids::kProductVersion))
            .Field("abi", mwb::ids::kAbiVersion)
            .EndObject();
        context.console.Emit(writer);
        return ExitCode::Success;
    }
};

class StatusCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"status"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        RejectArguments(context);
        const ProductRegistry registry;
        const std::optional<ProductRegistration> product = registry.ReadProduct();
        const std::optional<CameraRegistration> camera = registry.ReadCamera();
        const MicDeviceState mic = MicDriver{}.QueryState();
        const OsVersion os = QueryOsVersion();

        JsonWriter writer;
        writer.BeginObject().Field("ok", true).Key("installDir");
        if (product) {
            writer.String(product->installDir);
        } else {
            writer.Null();
        }

        writer.Key("os").BeginObject().Field("build", os.build).Field("isWin11", os.IsWindows11OrLater()).EndObject();

        writer.Key("camera").BeginObject().Field("installed", camera.has_value());
        if (camera) {
            writer.Field("backend", ToString(camera->backend))
                .Field("friendlyName", camera->friendlyName)
                .Field("width", camera->mode.width)
                .Field("height", camera->mode.height)
                .Field("fpsNum", camera->mode.fpsNum)
                .Field("fpsDen", camera->mode.fpsDen)
                .Field("pipeName", camera->pipeName);
        }
        writer.EndObject();

        writer.Key("mic").BeginObject().Field("installed", mic.installed);
        if (mic.installed) writer.Field("devicePresent", mic.present).Field("problemCode", mic.problemCode);
        writer.EndObject();

        writer.EndObject();
        context.console.Emit(writer);
        return ExitCode::Success;
    }
};

class DoctorCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"doctor"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        RejectArguments(context);
        const ProductRegistry registry;
        DoctorContext facts;
        facts.os = QueryOsVersion();
        facts.product = registry.ReadProduct();
        facts.camera = registry.ReadCamera();
        facts.mic = MicDriver{}.QueryState();

        struct Finding {
            std::string_view id;
            CheckResult result;
        };
        std::vector<Finding> findings;
        bool ok = true;
        const std::vector<std::unique_ptr<DoctorCheck>> checks = CreateDoctorChecks();
        for (const std::unique_ptr<DoctorCheck>& check : checks) {
            CheckResult result;
            try {
                result = check->Run(facts);
            } catch (...) {
                result = CheckResult{CheckStatus::Fail, "check failed: " + CurrentExceptionToCommandError().Message()};
            }
            ok = ok && result.status != CheckStatus::Fail;
            findings.push_back(Finding{check->Id(), std::move(result)});
        }

        JsonWriter writer;
        writer.BeginObject().Field("ok", ok).Key("checks").BeginArray();
        for (const Finding& finding : findings) {
            writer.BeginObject()
                .Field("id", finding.id)
                .Field("status", ToString(finding.result.status))
                .Field("message", finding.result.message)
                .EndObject();
        }
        writer.EndArray().EndObject();
        context.console.Emit(writer);
        return ExitCode::Success;  // a failing check is a finding, not a command failure
    }
};

}  // namespace

std::unique_ptr<Command> MakeVersionCommand() { return std::make_unique<VersionCommand>(); }
std::unique_ptr<Command> MakeStatusCommand() { return std::make_unique<StatusCommand>(); }
std::unique_ptr<Command> MakeDoctorCommand() { return std::make_unique<DoctorCommand>(); }

}  // namespace mwb::native
