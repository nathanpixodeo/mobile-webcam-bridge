#include "core/Command.h"

#include "core/Json.h"
#include "core/Output.h"
#include "core/Strings.h"

namespace mwb::native {

namespace {

std::vector<std::wstring_view> SplitWords(std::wstring_view path) {
    std::vector<std::wstring_view> words;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find(L' ', start);
        const std::size_t stop = end == std::wstring_view::npos ? path.size() : end;
        if (stop > start) words.push_back(path.substr(start, stop - start));
        start = stop + 1;
    }
    return words;
}

}  // namespace

void WriteErrorObject(JsonWriter& writer, const CommandError& error) {
    writer.BeginObject().Field("code", ToString(error.Code())).Field("message", error.Message()).EndObject();
}

void Application::Register(std::unique_ptr<Command> command) { commands_.push_back(std::move(command)); }

Application::Resolution Application::Resolve(std::span<const std::wstring> args) const {
    std::size_t positional = 0;
    while (positional < args.size() && !StartsWith(args[positional], L"--")) ++positional;

    Resolution best;
    for (const auto& command : commands_) {
        const std::vector<std::wstring_view> words = SplitWords(command->Path());
        if (words.size() > positional || words.size() <= best.consumedWords) continue;
        bool matches = true;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i] != args[i]) {
                matches = false;
                break;
            }
        }
        if (matches) best = Resolution{command.get(), words.size()};
    }
    return best;
}

std::string Application::UsageText() const {
    std::string text = "usage: bridge-native <command> [options]; commands:";
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        text += i == 0 ? " " : ", ";
        text += ToUtf8(commands_[i]->Path());
    }
    return text;
}

int Application::Run(std::span<const std::wstring> args) {
    const Resolution resolution = Resolve(args);
    if (resolution.command == nullptr) {
        ReportFailure(CommandError(ErrorCode::Usage, UsageText()), OutputStyle::OneShot);
        return static_cast<int>(ExitCode::Usage);
    }

    const CommandContext context{args.subspan(resolution.consumedWords), console_};
    try {
        return static_cast<int>(resolution.command->Run(context));
    } catch (...) {
        const CommandError error = CurrentExceptionToCommandError();
        ReportFailure(error, resolution.command->Style());
        return static_cast<int>(error.Exit());
    }
}

void Application::ReportFailure(const CommandError& error, OutputStyle style) {
    JsonWriter writer;
    if (style == OutputStyle::OneShot) {
        writer.BeginObject().Field("ok", false).Key("error");
        WriteErrorObject(writer, error);
        writer.EndObject();
    } else {
        writer.BeginObject()
            .Field("event", "error")
            .Field("code", ToString(error.Code()))
            .Field("message", error.Message())
            .Field("fatal", true)
            .EndObject();
    }
    console_.Emit(writer);
    console_.Error(error.Message());
}

}  // namespace mwb::native
