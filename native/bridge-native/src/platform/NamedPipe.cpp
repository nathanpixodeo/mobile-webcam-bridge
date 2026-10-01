#include "platform/NamedPipe.h"

#include <algorithm>

#include <wil/result.h>

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

OverlappedOperation::OverlappedOperation() {
    event_.create(wil::EventOptions::ManualReset);
    overlapped_.hEvent = event_.get();
}

void OverlappedOperation::Reset() noexcept {
    overlapped_ = OVERLAPPED{};
    overlapped_.hEvent = event_.get();
    event_.ResetEvent();
}

IoResult FinishIo(HANDLE file, OverlappedOperation& operation, BOOL started, HANDLE stop, DWORD timeoutMs) {
    if (!started) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_IO_PENDING) return IoResult{IoStatus::Failed, 0, error};

        const HANDLE handles[2] = {operation.Event(), stop};
        const DWORD count = stop != nullptr ? 2 : 1;
        const DWORD wait = ::WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
        if (wait != WAIT_OBJECT_0) {
            // Cancel and wait for the cancellation, so the OVERLAPPED is no longer referenced.
            ::CancelIoEx(file, operation.Get());
            DWORD ignored = 0;
            ::GetOverlappedResult(file, operation.Get(), &ignored, TRUE);
            if (wait == WAIT_OBJECT_0 + 1) return IoResult{IoStatus::Stopped, 0, ERROR_OPERATION_ABORTED};
            if (wait == WAIT_TIMEOUT) return IoResult{IoStatus::TimedOut, 0, ERROR_TIMEOUT};
            return IoResult{IoStatus::Failed, 0, ::GetLastError()};
        }
    }

    DWORD bytes = 0;
    if (!::GetOverlappedResult(file, operation.Get(), &bytes, FALSE)) {
        return IoResult{IoStatus::Failed, bytes, ::GetLastError()};
    }
    return IoResult{IoStatus::Completed, bytes, ERROR_SUCCESS};
}

IoResult ReadSome(HANDLE file, OverlappedOperation& operation, void* buffer, DWORD size, HANDLE stop, DWORD timeoutMs) {
    operation.Reset();
    const BOOL started = ::ReadFile(file, buffer, size, nullptr, operation.Get());
    return FinishIo(file, operation, started, stop, timeoutMs);
}

IoResult WriteAll(HANDLE file, OverlappedOperation& operation, const void* buffer, DWORD size, HANDLE stop,
                  DWORD timeoutMs) {
    const auto* bytes = static_cast<const BYTE*>(buffer);
    DWORD total = 0;
    while (total < size) {
        operation.Reset();
        const BOOL started = ::WriteFile(file, bytes + total, size - total, nullptr, operation.Get());
        IoResult result = FinishIo(file, operation, started, stop, timeoutMs);
        if (result.status != IoStatus::Completed) {
            result.bytes = total;
            return result;
        }
        if (result.bytes == 0) return IoResult{IoStatus::Failed, total, ERROR_WRITE_FAULT};
        total += result.bytes;
    }
    return IoResult{IoStatus::Completed, total, ERROR_SUCCESS};
}

IoResult ReadExactly(HANDLE file, OverlappedOperation& operation, void* buffer, DWORD size, HANDLE stop,
                     DWORD timeoutMs) {
    auto* bytes = static_cast<BYTE*>(buffer);
    DWORD total = 0;
    const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : ::GetTickCount64() + timeoutMs;
    while (total < size) {
        DWORD remaining = INFINITE;
        if (timeoutMs != INFINITE) {
            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline) return IoResult{IoStatus::TimedOut, total, ERROR_TIMEOUT};
            remaining = static_cast<DWORD>(deadline - now);
        }
        IoResult result = ReadSome(file, operation, bytes + total, size - total, stop, remaining);
        if (result.status != IoStatus::Completed) {
            result.bytes = total;
            return result;
        }
        total += result.bytes;
    }
    return IoResult{IoStatus::Completed, total, ERROR_SUCCESS};
}

IoResult AwaitClient(HANDLE pipe, OverlappedOperation& operation, HANDLE stop) {
    operation.Reset();
    const BOOL started = ::ConnectNamedPipe(pipe, operation.Get());
    if (!started && ::GetLastError() == ERROR_PIPE_CONNECTED) return IoResult{IoStatus::Completed, 0, ERROR_SUCCESS};
    return FinishIo(pipe, operation, started, stop, INFINITE);
}

wil::unique_handle CreatePipeServer(const std::wstring& path, const PipeServerOptions& options,
                                    SECURITY_ATTRIBUTES* security) {
    const DWORD openMode =
        options.openMode | FILE_FLAG_OVERLAPPED | (options.firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    const DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    // CreateNamedPipeW and CreateFileW report failure with INVALID_HANDLE_VALUE, not null.
    const HANDLE pipe = ::CreateNamedPipeW(path.c_str(), openMode, pipeMode, options.maxInstances,
                                           options.outBufferBytes, options.inBufferBytes, 0, security);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY) {
            throw CommandError(ErrorCode::PipeInUse, ToUtf8(path) + " is already in use by another process");
        }
        throw ErrorFromWin32(error, "Cannot create " + ToUtf8(path));
    }
    return wil::unique_handle(pipe);
}

wil::unique_handle OpenPipeClient(const std::wstring& path, DWORD access, DWORD timeoutMs, DWORD& error) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (true) {
        const HANDLE pipe = ::CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            error = ERROR_SUCCESS;
            return wil::unique_handle(pipe);
        }
        error = ::GetLastError();
        if (error != ERROR_PIPE_BUSY) return {};
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return {};
        ::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 1000)));
    }
}

}  // namespace mwb::native
