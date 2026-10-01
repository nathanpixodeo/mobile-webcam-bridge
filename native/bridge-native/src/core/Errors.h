// Error model of bridge-native: every failure that reaches the user is a CommandError carrying
// a contract error code (protocol/BRIDGE_NATIVE.md §1) and the process exit code.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace mwb::native {

enum class ExitCode : int {
    Success = 0,
    Failure = 1,
    Usage = 2,
    NotInstalled = 3,
    ElevationCancelled = 4,
    NotSupportedOs = 5,
};

enum class ErrorCode {
    Usage,
    NotInstalled,
    DeviceNotPresent,
    DeviceBusy,
    ElevationCancelled,
    NotSupportedOs,
    AccessDenied,
    PipeInUse,
    IoError,
    Internal,
};

[[nodiscard]] std::string_view ToString(ErrorCode code) noexcept;
[[nodiscard]] ExitCode DefaultExitCode(ErrorCode code) noexcept;

class CommandError : public std::runtime_error {
public:
    CommandError(ErrorCode code, std::string message);
    CommandError(ErrorCode code, ExitCode exitCode, std::string message);

    [[nodiscard]] ErrorCode Code() const noexcept { return code_; }
    [[nodiscard]] ExitCode Exit() const noexcept { return exitCode_; }
    [[nodiscard]] const std::string& Message() const noexcept { return message_; }

private:
    ErrorCode code_;
    ExitCode exitCode_;
    std::string message_;
};

// Human-readable text for a Win32 error / HRESULT (UTF-8, trailing whitespace trimmed).
[[nodiscard]] std::string DescribeWin32Error(unsigned long error);
[[nodiscard]] std::string DescribeHresult(long hr);

// Builds a CommandError from a Win32 error, choosing the contract code from the error value.
[[nodiscard]] CommandError ErrorFromWin32(unsigned long error, std::string_view context);
[[nodiscard]] CommandError ErrorFromHresult(long hr, std::string_view context);

// Converts whatever is in flight (CommandError, wil::ResultException, std::exception) into a
// CommandError. Call only from inside a catch block.
[[nodiscard]] CommandError CurrentExceptionToCommandError();

}  // namespace mwb::native
