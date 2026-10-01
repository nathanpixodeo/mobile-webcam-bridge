#include "platform/Process.h"

#include <windows.h>

#include <wil/resource.h>
#include <wil/result.h>

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

bool IsProcessElevated() {
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    THROW_IF_WIN32_BOOL_FALSE(::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size));
    return elevation.TokenIsElevated != 0;
}

std::wstring QuoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    // Rules of CommandLineToArgvW: backslashes are literal unless they precede a quote, in
    // which case they must be doubled; a quote is escaped with one backslash.
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring JoinArguments(std::span<const std::wstring> arguments) {
    std::wstring line;
    for (const std::wstring& argument : arguments) {
        if (!line.empty()) line.push_back(L' ');
        line += QuoteArgument(argument);
    }
    return line;
}

unsigned long RunAndWait(const std::filesystem::path& executable, std::span<const std::wstring> arguments,
                         unsigned long timeoutMs) {
    std::wstring commandLine = QuoteArgument(executable.native());
    if (!arguments.empty()) commandLine += L" " + JoinArguments(arguments);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wil::unique_process_information process;
    if (!::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process)) {
        throw ErrorFromWin32(::GetLastError(), "Cannot start " + ToUtf8(executable.filename().native()));
    }

    const DWORD wait = ::WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        throw CommandError(ErrorCode::Internal, ToUtf8(executable.filename().native()) + " did not finish in time");
    }
    DWORD exitCode = 0;
    THROW_IF_WIN32_BOOL_FALSE(::GetExitCodeProcess(process.hProcess, &exitCode));
    return exitCode;
}

}  // namespace mwb::native
