// Command abstraction. Each subcommand of bridge-native ("status", "video hub", ...) is one
// Command registered with the Application, which resolves the command words and reports errors
// in the output style the contract prescribes for that command.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Errors.h"

namespace mwb::native {

class Console;
class JsonWriter;

enum class OutputStyle {
    OneShot,  // exactly one JSON object on stdout: {"ok": ...}
    Stream,   // JSON lines on stdout: {"event": ...}
};

struct CommandContext {
    std::span<const std::wstring> args;  // everything after the command words
    Console& console;
};

class Command {
public:
    virtual ~Command() = default;

    [[nodiscard]] virtual std::wstring_view Path() const = 0;  // e.g. L"video hub"
    [[nodiscard]] virtual OutputStyle Style() const = 0;

    // Returns the process exit code; throws CommandError on failure.
    virtual ExitCode Run(const CommandContext& context) = 0;
};

// Writes {"code":..., "message":...} for `error` as the current value.
void WriteErrorObject(JsonWriter& writer, const CommandError& error);

class Application {
public:
    explicit Application(Console& console) noexcept : console_(console) {}

    void Register(std::unique_ptr<Command> command);
    [[nodiscard]] int Run(std::span<const std::wstring> args);

private:
    struct Resolution {
        Command* command = nullptr;
        std::size_t consumedWords = 0;
    };

    [[nodiscard]] Resolution Resolve(std::span<const std::wstring> args) const;
    [[nodiscard]] std::string UsageText() const;
    void ReportFailure(const CommandError& error, OutputStyle style);

    Console& console_;
    std::vector<std::unique_ptr<Command>> commands_;
};

}  // namespace mwb::native
