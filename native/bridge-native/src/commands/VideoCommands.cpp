// video hub (long-running) and video watch (one-shot diagnostics).
#include "commands/Commands.h"
#include "core/Json.h"
#include "core/Output.h"
#include "video/FrameWatcher.h"
#include "video/HubSettings.h"
#include "video/VideoHub.h"

namespace mwb::native {

namespace {

class VideoHubCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"video hub"; }
    OutputStyle Style() const override { return OutputStyle::Stream; }

    ExitCode Run(const CommandContext& context) override {
        HubSettings settings = ToHubSettings(HubArgParser().Parse(context.args));
        VideoHub hub(context.console, std::move(settings));
        return hub.Run();
    }
};

class VideoWatchCommand final : public Command {
public:
    std::wstring_view Path() const override { return L"video watch"; }
    OutputStyle Style() const override { return OutputStyle::OneShot; }

    ExitCode Run(const CommandContext& context) override {
        const WatchReport report = FrameWatcher(ToWatchSettings(WatchArgParser().Parse(context.args))).Run();
        JsonWriter writer;
        writer.BeginObject()
            .Field("ok", true)
            .Field("frames", report.frames)
            .Field("placeholderFrames", report.placeholderFrames)
            .Field("width", report.width)
            .Field("height", report.height)
            .Field("elapsedMs", report.elapsedMs)
            .EndObject();
        context.console.Emit(writer);
        return ExitCode::Success;
    }
};

}  // namespace

std::unique_ptr<Command> MakeVideoHubCommand() { return std::make_unique<VideoHubCommand>(); }
std::unique_ptr<Command> MakeVideoWatchCommand() { return std::make_unique<VideoWatchCommand>(); }

}  // namespace mwb::native
