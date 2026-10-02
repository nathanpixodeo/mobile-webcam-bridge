#include <mwb/um/PipeFrameReceiver.h>

#include <mwb/um/CameraConfig.h>
#include <mwb/um/Tracing.h>

#include <wil/result_macros.h>

#include <algorithm>

namespace mwb::um {

namespace {

constexpr DWORD kInitialBackoffMs = 250;
constexpr DWORD kMaxBackoffMs = 2000;
constexpr DWORD kPipeBusyRetryMs = 50;

const wchar_t* Describe(frame::ValidationError error) noexcept {
    switch (error) {
        case frame::ValidationError::None: return L"payload size";
        case frame::ValidationError::BadMagic: return L"bad magic";
        case frame::ValidationError::BadVersion: return L"bad version";
        case frame::ValidationError::BadHeaderSize: return L"bad header size";
        case frame::ValidationError::BadFourcc: return L"bad fourcc";
        case frame::ValidationError::ModeMismatch: return L"mode mismatch";
        case frame::ValidationError::BadStride: return L"bad stride";
        case frame::ValidationError::BadPayloadSize: return L"bad payload size";
        case frame::ValidationError::UnknownFlags: return L"unknown flags";
    }
    return L"unknown";
}

}  // namespace

PipeFrameReceiver::PipeFrameReceiver(std::wstring_view pipeName, const frame::VideoMode& mode)
    : pipePath_(PipePath(pipeName)), mode_(mode), frameBytes_(frame::Nv12FrameBytes(mode.width, mode.height)) {}

PipeFrameReceiver::~PipeFrameReceiver() {
    Stop();
}

HRESULT PipeFrameReceiver::Start() noexcept try {
    if (thread_.joinable()) return S_OK;
    RETURN_HR_IF(E_INVALIDARG, !frame::IsSupportedMode(mode_));

    if (!stopEvent_) RETURN_IF_FAILED(stopEvent_.create(wil::EventOptions::ManualReset));
    if (!frameEvent_) RETURN_IF_FAILED(frameEvent_.create(wil::EventOptions::None));
    if (!ioEvent_) RETURN_IF_FAILED(ioEvent_.create(wil::EventOptions::ManualReset));
    stopEvent_.ResetEvent();

    // Size every slot up front so the receive thread never allocates while streaming.
    frames_.ForEachSlotUnsynchronized([this](ReceivedFrame& slot) { slot.nv12.resize(frameBytes_); });

    thread_ = std::thread(&PipeFrameReceiver::Run, this);
    return S_OK;
}
CATCH_RETURN()

void PipeFrameReceiver::Stop() noexcept {
    if (!thread_.joinable()) return;
    stopEvent_.SetEvent();
    thread_.join();
    connected_.store(false, std::memory_order_release);
}

const ReceivedFrame& PipeFrameReceiver::AcquireLatest(bool* isNew) noexcept {
    const bool acquired = frames_.Acquire();
    if (isNew != nullptr) *isNew = acquired;
    return frames_.ReadSlot();
}

bool PipeFrameReceiver::StopRequested(DWORD waitMilliseconds) const noexcept {
    return WaitForSingleObject(stopEvent_.get(), waitMilliseconds) == WAIT_OBJECT_0;
}

wil::unique_hfile PipeFrameReceiver::Connect() const noexcept {
    // GENERIC_READ plus FILE_WRITE_DATA for the subscription: exactly what the hub's DACL grants
    // LocalService (Frame Server). SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS stops the pipe server
    // from impersonating this (possibly privileged) client.
    return wil::unique_hfile(CreateFileW(pipePath_.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
                                         nullptr));
}

void PipeFrameReceiver::Run() noexcept {
    DWORD backoffMs = kInitialBackoffMs;
    bool reportedUnavailable = false;

    while (!StopRequested(0)) {
        wil::unique_hfile pipe = Connect();
        if (!pipe) {
            const DWORD error = GetLastError();
            if (!reportedUnavailable) {
                trace::Writef(trace::Level::Info, L"Frame pipe {} unavailable (error {}), retrying", pipePath_, error);
                reportedUnavailable = true;
            }
            if (StopRequested(error == ERROR_PIPE_BUSY ? kPipeBusyRetryMs : backoffMs)) break;
            backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
            continue;
        }

        reportedUnavailable = false;
        connected_.store(true, std::memory_order_release);
        trace::Writef(trace::Level::Info, L"Connected to frame pipe {}", pipePath_);

        const std::uint64_t framesBefore = FramesReceived();
        const SessionEnd end = ReceiveFrames(pipe.get());
        pipe.reset();
        connected_.store(false, std::memory_order_release);
        if (end == SessionEnd::Stopped) break;

        trace::Writef(trace::Level::Info, L"Frame pipe session ended ({})",
                      end == SessionEnd::ProtocolError ? L"protocol error" : L"disconnected");
        // A session that delivered frames was healthy: reconnect quickly. A server that keeps
        // sending garbage is throttled by the growing backoff instead.
        backoffMs = FramesReceived() > framesBefore ? kInitialBackoffMs : std::min(backoffMs * 2, kMaxBackoffMs);
        if (StopRequested(backoffMs)) break;
    }
}

PipeFrameReceiver::SessionEnd PipeFrameReceiver::ReceiveFrames(HANDLE pipe) noexcept {
    const frame::SubscribeRequest request = frame::MakeSubscribeRequest(mode_);
    if (!WriteExact(pipe, &request, sizeof(request))) {
        return StopRequested(0) ? SessionEnd::Stopped : SessionEnd::Disconnected;
    }

    for (;;) {
        frame::FrameHeader header{};
        if (!ReadExact(pipe, &header, sizeof(header))) {
            return StopRequested(0) ? SessionEnd::Stopped : SessionEnd::Disconnected;
        }

        const frame::ValidationError error = frame::Validate(header, mode_.width, mode_.height);
        if (error != frame::ValidationError::None || header.payloadSize != frameBytes_) {
            trace::Writef(trace::Level::Warning, L"Rejected frame header ({}): {}x{}, {} bytes",
                          Describe(error), header.width, header.height, header.payloadSize);
            return SessionEnd::ProtocolError;
        }

        ReceivedFrame& slot = frames_.WriteSlot();
        if (!ReadExact(pipe, slot.nv12.data(), header.payloadSize)) {
            slot.valid = false;
            return StopRequested(0) ? SessionEnd::Stopped : SessionEnd::Disconnected;
        }
        slot.seq = header.seq;
        slot.producerQpc100ns = header.producerQpc100ns;
        slot.flags = header.flags;
        slot.valid = true;

        frames_.Publish();
        framesReceived_.fetch_add(1, std::memory_order_relaxed);
        frameEvent_.SetEvent();
    }
}

bool PipeFrameReceiver::ReadExact(HANDLE pipe, void* destination, std::uint32_t length) noexcept {
    auto* cursor = static_cast<std::uint8_t*>(destination);
    std::uint32_t remaining = length;

    while (remaining > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent_.get();
        ioEvent_.ResetEvent();

        if (!ReadFile(pipe, cursor, remaining, nullptr, &overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) return false;
        }

        DWORD transferred = 0;
        if (!Complete(pipe, overlapped, &transferred)) return false;
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

bool PipeFrameReceiver::WriteExact(HANDLE pipe, const void* source, std::uint32_t length) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(source);
    std::uint32_t remaining = length;

    while (remaining > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent_.get();
        ioEvent_.ResetEvent();

        if (!WriteFile(pipe, cursor, remaining, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }

        DWORD transferred = 0;
        if (!Complete(pipe, overlapped, &transferred)) return false;
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

bool PipeFrameReceiver::Complete(HANDLE pipe, OVERLAPPED& overlapped, DWORD* transferred) noexcept {
    const HANDLE waits[] = {ioEvent_.get(), stopEvent_.get()};
    if (WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
        // Stop requested (or wait failure): cancel and wait for the cancellation to land, since
        // `overlapped` lives on the caller's stack frame.
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        return false;
    }

    if (!GetOverlappedResult(pipe, &overlapped, transferred, FALSE) && GetLastError() != ERROR_MORE_DATA) {
        return false;  // ERROR_BROKEN_PIPE when the hub goes away
    }
    return *transferred != 0;
}

}  // namespace mwb::um
