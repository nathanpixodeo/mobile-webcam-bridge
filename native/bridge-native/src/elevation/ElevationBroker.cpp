#include "elevation/ElevationBroker.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#include <wil/resource.h>

#include "core/Json.h"
#include "core/Output.h"
#include "core/Strings.h"
#include "platform/NamedPipe.h"
#include "platform/Paths.h"
#include "platform/Process.h"
#include "platform/Security.h"

namespace mwb::native {

namespace {

constexpr std::wstring_view kResultPipePrefix = L"\\\\.\\pipe\\mobile-webcam-bridge-result-";
constexpr std::size_t kTokenBytes = 16;  // 32 hex characters
// Generous: installing a test-signed driver can show a Windows Security dialog that waits for
// the user.
constexpr DWORD kElevatedTimeoutMs = 15 * 60 * 1000;
constexpr std::size_t kMaxResultBytes = 1 << 20;

ExitCode ToExitCode(DWORD code) noexcept {
    return code <= static_cast<DWORD>(ExitCode::NotSupportedOs) ? static_cast<ExitCode>(code) : ExitCode::Failure;
}

void ValidateResult(const std::string& json) {
    try {
        const JsonValue value = ParseJson(json, JsonParseLimits{16, kMaxResultBytes});
        const JsonValue* ok = value.Find("ok");
        if (ok == nullptr || ok->AsBool() == nullptr) {
            throw CommandError(ErrorCode::Internal, "The elevated installer returned JSON without \"ok\"");
        }
    } catch (const JsonParseError& error) {
        throw CommandError(ErrorCode::Internal, std::string("The elevated installer returned malformed output: ") + error.what());
    }
}

}  // namespace

bool ElevationBroker::IsValidResultPipe(std::wstring_view path) noexcept {
    return StartsWith(path, kResultPipePrefix) &&
           IsLowerHex(path.substr(kResultPipePrefix.size()), kTokenBytes * 2, kTokenBytes * 2);
}

ExitCode ElevationBroker::RunElevated(std::wstring_view commandWords, std::span<const std::wstring> args) {
    const std::wstring pipePath = std::wstring(kResultPipePrefix) + RandomHexToken(kTokenBytes);
    SecurityAttributes security(PrivatePipeSddl(CurrentUserSid()));
    PipeServerOptions options;
    options.openMode = PIPE_ACCESS_INBOUND;
    options.firstInstance = true;
    options.maxInstances = 1;
    options.outBufferBytes = 4 * 1024;
    options.inBufferBytes = 64 * 1024;
    const wil::unique_handle pipe = CreatePipeServer(pipePath, options, security.Get());

    // Start listening before the child exists, so its connect can never race us.
    OverlappedOperation connect;
    const BOOL connectStarted = ::ConnectNamedPipe(pipe.get(), connect.Get());
    const DWORD connectError = connectStarted ? ERROR_SUCCESS : ::GetLastError();
    bool connected = connectStarted != FALSE || connectError == ERROR_PIPE_CONNECTED;
    if (!connected && connectError != ERROR_IO_PENDING) throw ErrorFromWin32(connectError, "Cannot listen on the result pipe");

    std::wstring parameters(commandWords);
    for (const std::wstring& arg : args) parameters += L" " + QuoteArgument(arg);
    parameters += L" --elevated --result-pipe " + QuoteArgument(pipePath);

    const std::wstring executable = ModulePath().native();
    const std::wstring directory = ModuleDirectory().native();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_HIDE;

    console_.Info("requesting administrator rights (UAC)");
    if (!::ShellExecuteExW(&info)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_CANCELLED) throw CommandError(ErrorCode::ElevationCancelled, "The administrator prompt was declined");
        throw ErrorFromWin32(error, "Cannot start the elevated installer");
    }
    if (info.hProcess == nullptr) throw CommandError(ErrorCode::Internal, "The elevated installer did not start");
    const wil::unique_handle process(info.hProcess);

    if (!connected) {
        const HANDLE waits[2] = {connect.Event(), process.get()};
        const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kElevatedTimeoutMs);
        DWORD ignored = 0;
        if (wait == WAIT_OBJECT_0) {
            if (!::GetOverlappedResult(pipe.get(), connect.Get(), &ignored, FALSE)) {
                throw ErrorFromWin32(::GetLastError(), "The elevated installer could not connect");
            }
            connected = true;
        } else {
            ::CancelIoEx(pipe.get(), connect.Get());
            ::GetOverlappedResult(pipe.get(), connect.Get(), &ignored, TRUE);
            if (wait == WAIT_OBJECT_0 + 1) {
                DWORD exitCode = 0;
                ::GetExitCodeProcess(process.get(), &exitCode);
                throw CommandError(ErrorCode::Internal, "The elevated installer exited with code " + std::to_string(exitCode) +
                                                            " without reporting a result");
            }
            throw CommandError(ErrorCode::Internal, "Timed out waiting for the elevated installer");
        }
    }

    std::string result;
    char buffer[4096];
    OverlappedOperation read;
    while (true) {
        const IoResult chunk = ReadSome(pipe.get(), read, buffer, sizeof(buffer), nullptr, kElevatedTimeoutMs);
        if (chunk.status == IoStatus::Completed) {
            result.append(buffer, chunk.bytes);
            if (result.size() > kMaxResultBytes) throw CommandError(ErrorCode::Internal, "The elevated installer result is too large");
            continue;
        }
        if (chunk.status == IoStatus::Failed && (chunk.error == ERROR_BROKEN_PIPE || chunk.error == ERROR_PIPE_NOT_CONNECTED)) break;
        throw ErrorFromWin32(chunk.error, "Reading the elevated installer result");
    }

    DWORD exitCode = static_cast<DWORD>(ExitCode::Failure);
    if (::WaitForSingleObject(process.get(), 30'000) != WAIT_OBJECT_0 || !::GetExitCodeProcess(process.get(), &exitCode)) {
        exitCode = static_cast<DWORD>(ExitCode::Failure);
    }

    ValidateResult(result);
    console_.Out().WriteLine(result);
    return ToExitCode(exitCode);
}

void ElevationBroker::SendResult(std::wstring_view resultPipe, std::string_view json) {
    if (!IsValidResultPipe(resultPipe)) throw CommandError(ErrorCode::Usage, "--result-pipe is not a valid result pipe");

    DWORD error = ERROR_SUCCESS;
    const wil::unique_handle pipe = OpenPipeClient(std::wstring(resultPipe), GENERIC_WRITE, 10'000, error);
    if (!pipe) throw ErrorFromWin32(error, "Cannot open the result pipe");

    OverlappedOperation write;
    const IoResult sent = WriteAll(pipe.get(), write, json.data(), static_cast<DWORD>(json.size()), nullptr, 30'000);
    if (sent.status != IoStatus::Completed) throw ErrorFromWin32(sent.error, "Cannot send the result");
}

}  // namespace mwb::native
