// mic feed (long-running) and mic status (one-shot).
#include "commands/Commands.h"
#include "core/ArgParser.h"
#include "core/Json.h"
#include "core/Output.h"
#include "mic/MicDevice.h"
#include "mic/MicFeeder.h"

namespace mwb::native {

namespace {

class MicFeedCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"mic feed"; }
    OutputStyle Style() const override { return OutputStyle::Stream; }

    ExitCode Run(const CommandContext& context) override {
        (void)ArgParser{}.Parse(context.args);
        return MicFeeder(context.console).Run();
    }
};

class MicStatusCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"mic status"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        (void)ArgParser{}.Parse(context.args);
        MicOpenError error = MicOpenError::None;
        DWORD win32Error = ERROR_SUCCESS;
        const std::optional<MicDevice> device = MicDevice::Open(error, win32Error);

        JsonWriter writer;
        writer.BeginObject().Field("ok", true);
        if (device) {
            const MicStatus status = device->QueryStatus();
            writer.Field("present", true)
                .Field("busy", false)
                .Field("bufferedBytes", status.bufferedBytes)
                .Field("streamActive", status.streamActive);
        } else if (error == MicOpenError::Busy) {
            writer.Field("present", true).Field("busy", true);  // a feeder holds the exclusive handle
        } else if (error == MicOpenError::NotPresent) {
            writer.Field("present", false).Field("busy", false);
        } else {
            throw MicOpenFailure(error, win32Error);
        }
        writer.EndObject();
        context.console.Emit(writer);
        return ExitCode::Success;
    }
};

}  // namespace

std::unique_ptr<Command> MakeMicFeedCommand() { return std::make_unique<MicFeedCommand>(); }
std::unique_ptr<Command> MakeMicStatusCommand() { return std::make_unique<MicStatusCommand>(); }

}  // namespace mwb::native
