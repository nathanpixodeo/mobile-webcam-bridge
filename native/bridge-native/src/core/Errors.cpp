#include "core/Errors.h"

#include <windows.h>

#include <cstdio>
#include <new>

#include <wil/result.h>

#include "core/Strings.h"

namespace mwb::native {

std::string_view ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Usage: return "USAGE";
        case ErrorCode::NotInstalled: return "NOT_INSTALLED";
        case ErrorCode::DeviceNotPresent: return "DEVICE_NOT_PRESENT";
        case ErrorCode::DeviceBusy: return "DEVICE_BUSY";
        case ErrorCode::ElevationCancelled: return "ELEVATION_CANCELLED";
        case ErrorCode::NotSupportedOs: return "NOT_SUPPORTED_OS";
        case ErrorCode::AccessDenied: return "ACCESS_DENIED";
        case ErrorCode::PipeInUse: return "PIPE_IN_USE";
        case ErrorCode::IoError: return "IO_ERROR";
        case ErrorCode::Internal: return "INTERNAL";
    }
    return "INTERNAL";
}

ExitCode DefaultExitCode(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Usage: return ExitCode::Usage;
        case ErrorCode::NotInstalled:
        case ErrorCode::DeviceNotPresent: return ExitCode::NotInstalled;
        case ErrorCode::ElevationCancelled: return ExitCode::ElevationCancelled;
        case ErrorCode::NotSupportedOs: return ExitCode::NotSupportedOs;
        case ErrorCode::DeviceBusy:
        case ErrorCode::AccessDenied:
        case ErrorCode::PipeInUse:
        case ErrorCode::IoError:
        case ErrorCode::Internal: return ExitCode::Failure;
    }
    return ExitCode::Failure;
}

CommandError::CommandError(ErrorCode code, std::string message)
    : CommandError(code, DefaultExitCode(code), std::move(message)) {}

CommandError::CommandError(ErrorCode code, ExitCode exitCode, std::string message)
    : std::runtime_error(message), code_(code), exitCode_(exitCode), message_(std::move(message)) {}

namespace {

std::string FormatSystemMessage(DWORD messageId) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        messageId, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) return {};
    std::wstring text(buffer, length);
    ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return ToUtf8(text);
}

ErrorCode CodeForWin32(DWORD error) noexcept {
    switch (error) {
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD: return ErrorCode::AccessDenied;
        case ERROR_CANCELLED: return ErrorCode::ElevationCancelled;
        case ERROR_SHARING_VIOLATION:
        case ERROR_BUSY: return ErrorCode::DeviceBusy;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_HANDLE_EOF:
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
        case ERROR_PIPE_BUSY:
        case ERROR_SEM_TIMEOUT:
        case ERROR_IO_DEVICE: return ErrorCode::IoError;
        default: return ErrorCode::Internal;
    }
}

}  // namespace

std::string DescribeWin32Error(unsigned long error) {
    std::string text = FormatSystemMessage(error);
    if (text.empty()) text = "Win32 error";
    return text + " (" + std::to_string(error) + ")";
}

std::string DescribeHresult(long hr) {
    std::string text = FormatSystemMessage(static_cast<DWORD>(hr));
    if (text.empty()) text = "HRESULT";
    char code[16] = {};
    ::sprintf_s(code, "0x%08lX", static_cast<unsigned long>(hr));
    return text + " (" + code + ")";
}

CommandError ErrorFromWin32(unsigned long error, std::string_view context) {
    return CommandError(CodeForWin32(error), std::string(context) + ": " + DescribeWin32Error(error));
}

CommandError ErrorFromHresult(long hr, std::string_view context) {
    ErrorCode code = ErrorCode::Internal;
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        code = CodeForWin32(static_cast<DWORD>(HRESULT_CODE(hr)));
    } else if (hr == E_ACCESSDENIED) {
        code = ErrorCode::AccessDenied;
    }
    return CommandError(code, std::string(context) + ": " + DescribeHresult(hr));
}

CommandError CurrentExceptionToCommandError() {
    try {
        throw;
    } catch (const CommandError& error) {
        return error;
    } catch (const wil::ResultException& error) {
        return ErrorFromHresult(error.GetErrorCode(), "System call failed");
    } catch (const std::bad_alloc&) {
        return CommandError(ErrorCode::Internal, "Out of memory");
    } catch (const std::exception& error) {
        return CommandError(ErrorCode::Internal, error.what());
    } catch (...) {
        return CommandError(ErrorCode::Internal, "Unknown error");
    }
}

}  // namespace mwb::native
