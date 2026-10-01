// Turns IMFMediaStream::RequestSample tokens into paced MEMediaSample events.
//
// The pipeline may request samples far faster than the camera's frame rate; answering each request
// immediately would spin a core and flood clients with duplicates. Instead requests are queued and
// a dedicated thread answers them:
//   * as soon as a new frame arrived, but never faster than 1.5 × the nominal frame rate;
//   * otherwise after one nominal frame interval, repeating the last frame (we are the clock).
// The thread never touches the owning stream's lock, so the stream may Stop() (and join) while
// holding it without deadlocking.
#pragma once

#include "Framework.h"
#include "MediaTypes.h"
#include "SampleWriter.h"

#include <mwb/FrameProtocol.h>
#include <mwb/um/PipeFrameReceiver.h>
#include <mwb/um/Placeholder.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mwb::vcam {

struct DeliveryConfig {
    wil::com_ptr_nothrow<IMFMediaEventQueue> eventQueue;
    wil::com_ptr_nothrow<IMFVideoSampleAllocator> allocator;
    frame::VideoMode mode{};
    OutputFormat format = OutputFormat::Nv12;
    std::wstring pipeName;
};

class FrameDelivery final {
public:
    explicit FrameDelivery(DeliveryConfig config);
    ~FrameDelivery();

    FrameDelivery(const FrameDelivery&) = delete;
    FrameDelivery& operator=(const FrameDelivery&) = delete;

    // Connects the frame pipe receiver and starts the delivery thread.
    [[nodiscard]] HRESULT Start() noexcept;

    // Stops the thread and the receiver; pending requests are discarded. Idempotent.
    void Stop() noexcept;

    // Queues one sample request (`token` may be null). Thread-safe.
    [[nodiscard]] HRESULT Enqueue(IUnknown* token) noexcept;

    // While paused, requests are kept but not answered. Thread-safe.
    void SetPaused(bool paused) noexcept;

private:
    using Token = wil::com_ptr_nothrow<IUnknown>;

    void Run() noexcept;
    // Pops the next request if one is due now; otherwise returns the milliseconds to wait.
    [[nodiscard]] bool TakeDueRequest(LONGLONG now, Token& token, DWORD& waitMs, bool& hasPending) noexcept;
    [[nodiscard]] HRESULT Deliver(IUnknown* token, LONGLONG now) noexcept;
    [[nodiscard]] const std::uint8_t* CurrentFrame() noexcept;
    void Requeue(Token token) noexcept;

    DeliveryConfig config_;
    const LONGLONG frameInterval_;
    const LONGLONG minInterval_;
    SampleWriter writer_;
    um::PipeFrameReceiver receiver_;
    um::PlaceholderGenerator placeholder_;
    std::vector<std::uint8_t> placeholderFrame_;  // delivery thread only
    std::uint64_t placeholderIndex_ = 0;          // delivery thread only
    LONGLONG lastDelivery_ = 0;                   // delivery thread only
    bool reportedDeliveryFailure_ = false;        // delivery thread only

    std::mutex mutex_;  // guards requests_ and paused_
    std::deque<Token> requests_;
    bool paused_ = false;

    wil::unique_event_nothrow wakeEvent_;  // auto-reset: request queued or pause toggled
    wil::unique_event_nothrow stopEvent_;  // manual-reset
    std::thread thread_;
};

}  // namespace mwb::vcam
