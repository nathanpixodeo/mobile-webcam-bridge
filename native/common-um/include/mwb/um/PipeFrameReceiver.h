// Client side of the public frame pipe (protocol/FRAME_PIPE.md).
//
// A private thread connects to `bridge-native video hub`, reads header + NV12 payload, validates
// every header against the installed camera mode and publishes only the newest frame through a
// lock-free triple buffer. Any protocol violation drops the connection; the thread reconnects with
// a 250 ms → 2 s backoff for as long as the receiver runs.
#pragma once

#include <mwb/FrameProtocol.h>
#include <mwb/TripleBuffer.h>

#include <windows.h>

#include <wil/resource.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mwb::um {

struct ReceivedFrame {
    std::vector<std::uint8_t> nv12;  // tightly packed, width × height × 3/2 bytes
    std::uint64_t seq = 0;
    std::uint64_t producerQpc100ns = 0;
    std::uint32_t flags = 0;
    bool valid = false;  // false until a frame has been received into this slot
};

class PipeFrameReceiver final {
public:
    // `pipeName` without the "\\.\pipe\" prefix; `mode` must satisfy frame::IsSupportedMode.
    PipeFrameReceiver(std::wstring_view pipeName, const frame::VideoMode& mode);
    ~PipeFrameReceiver();

    PipeFrameReceiver(const PipeFrameReceiver&) = delete;
    PipeFrameReceiver& operator=(const PipeFrameReceiver&) = delete;

    // Allocates the frame slots and starts the receive thread. Idempotent.
    [[nodiscard]] HRESULT Start() noexcept;

    // Cancels pending I/O, joins the thread and closes the pipe. Idempotent; must not be called
    // from the receive thread itself.
    void Stop() noexcept;

    // ---- Consumer side: exactly one consumer thread ----

    // Auto-reset event, signalled each time a frame is published.
    [[nodiscard]] HANDLE FrameAvailableEvent() const noexcept { return frameEvent_.get(); }

    // True when a frame newer than the one returned by the last AcquireLatest() is waiting.
    [[nodiscard]] bool HasFreshFrame() const noexcept { return frames_.HasFresh(); }

    // Takes the newest published frame (if any) and returns the consumer's current frame.
    // The reference stays valid until the next AcquireLatest() call. Check `valid`.
    [[nodiscard]] const ReceivedFrame& AcquireLatest(bool* isNew = nullptr) noexcept;

    // ---- Any thread ----
    [[nodiscard]] bool IsConnected() const noexcept { return connected_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t FramesReceived() const noexcept { return framesReceived_.load(std::memory_order_relaxed); }

private:
    enum class SessionEnd { Stopped, Disconnected, ProtocolError };

    void Run() noexcept;
    [[nodiscard]] bool StopRequested(DWORD waitMilliseconds) const noexcept;
    [[nodiscard]] wil::unique_hfile Connect() const noexcept;
    [[nodiscard]] SessionEnd ReceiveFrames(HANDLE pipe) noexcept;
    // Reads exactly `length` bytes; false on disconnect, I/O error or stop request.
    [[nodiscard]] bool ReadExact(HANDLE pipe, void* destination, std::uint32_t length) noexcept;

    const std::wstring pipePath_;
    const frame::VideoMode mode_;
    const std::uint32_t frameBytes_;

    TripleBuffer<ReceivedFrame> frames_;
    wil::unique_event_nothrow stopEvent_;   // manual-reset
    wil::unique_event_nothrow frameEvent_;  // auto-reset
    wil::unique_event_nothrow ioEvent_;     // manual-reset, owned by the receive thread
    std::thread thread_;

    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> framesReceived_{0};
};

}  // namespace mwb::um
