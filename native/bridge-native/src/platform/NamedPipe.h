// Overlapped named-pipe primitives shared by the video hub, `video watch` and the elevation
// broker. Every wait also watches a stop event, so shutdown never hangs on blocked I/O.
#pragma once

#include <windows.h>

#include <string>

#include <wil/resource.h>

namespace mwb::native {

// An OVERLAPPED structure bound to its own manual-reset event.
class OverlappedOperation {
public:
    OverlappedOperation();
    OverlappedOperation(const OverlappedOperation&) = delete;
    OverlappedOperation& operator=(const OverlappedOperation&) = delete;

    // Clears the structure for reuse (the event stays bound).
    void Reset() noexcept;
    [[nodiscard]] OVERLAPPED* Get() noexcept { return &overlapped_; }
    [[nodiscard]] HANDLE Event() const noexcept { return event_.get(); }

private:
    wil::unique_event event_;
    OVERLAPPED overlapped_{};
};

enum class IoStatus {
    Completed,  // operation finished successfully
    Failed,     // operation finished with an error (see IoResult::error)
    Stopped,    // stop event signalled; the operation was cancelled
    TimedOut,   // timeout elapsed; the operation was cancelled
};

struct IoResult {
    IoStatus status = IoStatus::Failed;
    DWORD bytes = 0;
    DWORD error = ERROR_SUCCESS;
};

// Completes an overlapped operation that was started on `file` with `operation`.
// `started` is the BOOL returned by ReadFile/WriteFile/ConnectNamedPipe.
// `stop` may be null; `timeoutMs` may be INFINITE.
[[nodiscard]] IoResult FinishIo(HANDLE file, OverlappedOperation& operation, BOOL started, HANDLE stop,
                                DWORD timeoutMs = INFINITE);

[[nodiscard]] IoResult ReadSome(HANDLE file, OverlappedOperation& operation, void* buffer, DWORD size, HANDLE stop,
                                DWORD timeoutMs = INFINITE);
[[nodiscard]] IoResult WriteAll(HANDLE file, OverlappedOperation& operation, const void* buffer, DWORD size,
                                HANDLE stop, DWORD timeoutMs = INFINITE);
// Reads exactly `size` bytes (or fails).
[[nodiscard]] IoResult ReadExactly(HANDLE file, OverlappedOperation& operation, void* buffer, DWORD size, HANDLE stop,
                                   DWORD timeoutMs = INFINITE);

// Waits for a client on a server pipe instance. Completed means connected.
[[nodiscard]] IoResult AwaitClient(HANDLE pipe, OverlappedOperation& operation, HANDLE stop);

struct PipeServerOptions {
    DWORD openMode = PIPE_ACCESS_DUPLEX;  // FILE_FLAG_OVERLAPPED is always added
    bool firstInstance = false;           // FILE_FLAG_FIRST_PIPE_INSTANCE
    DWORD maxInstances = PIPE_UNLIMITED_INSTANCES;
    DWORD outBufferBytes = 64 * 1024;
    DWORD inBufferBytes = 4 * 1024;
};

// Creates one instance of a byte-mode server pipe that rejects remote clients.
// Throws CommandError (PIPE_IN_USE when the name is already taken by another server).
[[nodiscard]] wil::unique_handle CreatePipeServer(const std::wstring& path, const PipeServerOptions& options,
                                                  SECURITY_ATTRIBUTES* security);

// Opens a pipe as a client with SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, so the server can
// never impersonate this process. Retries ERROR_PIPE_BUSY until `timeoutMs` elapses.
// Returns an empty handle with `error` set when the pipe cannot be opened.
[[nodiscard]] wil::unique_handle OpenPipeClient(const std::wstring& path, DWORD access, DWORD timeoutMs, DWORD& error);

[[nodiscard]] inline std::wstring PipePath(const std::wstring& name) { return L"\\\\.\\pipe\\" + name; }

}  // namespace mwb::native
