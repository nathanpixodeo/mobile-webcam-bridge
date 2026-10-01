// Runs `install` / `uninstall` in an elevated copy of this executable (one UAC prompt) and
// relays its JSON result.
//
// The non-elevated parent creates a private result pipe, relaunches itself with the "runas" verb
// and the hidden options `--elevated --result-pipe <path>`, then prints whatever JSON the child
// sends and exits with the child's exit code. The pipe name is random and validated by the child,
// so an elevated process never writes to a path chosen by anything but this protocol.
#pragma once

#include <span>
#include <string>
#include <string_view>

#include "core/Errors.h"

namespace mwb::native {

class Console;

class ElevationBroker {
public:
    explicit ElevationBroker(Console& console) noexcept : console_(console) {}

    // Parent side. `commandWords` e.g. {L"install"}; `args` are the options the user passed.
    // Prints the child's JSON on stdout and returns the child's exit code.
    [[nodiscard]] ExitCode RunElevated(std::wstring_view commandWords, std::span<const std::wstring> args);

    // Child side: delivers the result JSON to the parent. Throws CommandError when the pipe name
    // is not a valid result pipe or the parent is gone.
    static void SendResult(std::wstring_view resultPipe, std::string_view json);

    [[nodiscard]] static bool IsValidResultPipe(std::wstring_view path) noexcept;

private:
    Console& console_;
};

}  // namespace mwb::native
