#include "Framework.h"

#include "FrameDelivery.h"

#include <mwb/um/Tracing.h>

#include <avrt.h>

namespace mwb::vcam {

namespace {

// How long to back off when every sample of the pipeline's pool is still downstream.
constexpr DWORD kAllocatorEmptyRetryMs = 2;
// Requests beyond this are stale: the pipeline never needs more than a handful outstanding.
constexpr std::size_t kMaxPendingRequests = 32;

DWORD ToWaitMilliseconds(LONGLONG duration100ns) noexcept {
    if (duration100ns <= 0) return 0;
    return static_cast<DWORD>((duration100ns + 9'999) / 10'000);  // round up
}

}  // namespace

FrameDelivery::FrameDelivery(DeliveryConfig config)
    : config_(std::move(config)),
      frameInterval_(FrameDuration(config_.mode)),
      minInterval_(FrameDuration(config_.mode) * 2 / 3),  // cap at 1.5 × the nominal rate
      writer_(config_.mode, config_.format),
      receiver_(config_.pipeName, config_.mode),
      placeholder_(config_.mode.width, config_.mode.height, config_.mode.fpsNum / config_.mode.fpsDen) {}

FrameDelivery::~FrameDelivery() {
    Stop();
}

HRESULT FrameDelivery::Start() noexcept try {
    if (thread_.joinable()) return S_OK;
    RETURN_HR_IF_NULL(E_UNEXPECTED, config_.eventQueue);
    RETURN_HR_IF_NULL(E_UNEXPECTED, config_.allocator);

    if (!wakeEvent_) RETURN_IF_FAILED(wakeEvent_.create(wil::EventOptions::None));
    if (!stopEvent_) RETURN_IF_FAILED(stopEvent_.create(wil::EventOptions::ManualReset));
    stopEvent_.ResetEvent();

    placeholderFrame_.resize(placeholder_.FrameBytes());
    RETURN_IF_FAILED(receiver_.Start());

    // Allow the very first request to be answered immediately (first sample well within 100 ms).
    lastDelivery_ = MFGetSystemTime() - frameInterval_;
    thread_ = std::thread(&FrameDelivery::Run, this);
    return S_OK;
}
CATCH_RETURN()

void FrameDelivery::Stop() noexcept {
    if (thread_.joinable()) {
        stopEvent_.SetEvent();
        thread_.join();
    }
    receiver_.Stop();

    std::deque<Token> discarded;
    {
        std::lock_guard lock(mutex_);
        discarded.swap(requests_);
    }
    // Tokens are released here, outside the lock.
}

HRESULT FrameDelivery::Enqueue(IUnknown* token) noexcept try {
    {
        std::lock_guard lock(mutex_);
        if (requests_.size() >= kMaxPendingRequests) requests_.pop_front();
        requests_.emplace_back(token);
    }
    wakeEvent_.SetEvent();
    return S_OK;
}
CATCH_RETURN()

void FrameDelivery::SetPaused(bool paused) noexcept {
    {
        std::lock_guard lock(mutex_);
        paused_ = paused;
    }
    wakeEvent_.SetEvent();
}

void FrameDelivery::Requeue(Token token) noexcept try {
    std::lock_guard lock(mutex_);
    requests_.emplace_front(std::move(token));
} catch (...) {
    // Out of memory: the request is dropped; the pipeline simply requests again.
}

bool FrameDelivery::TakeDueRequest(LONGLONG now, Token& token, DWORD& waitMs, bool& hasPending) noexcept {
    std::lock_guard lock(mutex_);
    hasPending = !requests_.empty() && !paused_;
    if (!hasPending) {
        waitMs = INFINITE;
        return false;
    }

    const LONGLONG elapsed = now - lastDelivery_;
    const LONGLONG due = receiver_.HasFreshFrame() ? minInterval_ : frameInterval_;
    if (elapsed < due) {
        waitMs = ToWaitMilliseconds(due - elapsed);
        return false;
    }

    token = std::move(requests_.front());
    requests_.pop_front();
    return true;
}

void FrameDelivery::Run() noexcept {
    // Join the MMCSS "Capture" class like other capture pipelines, so pacing survives CPU load.
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    auto revertMmcss = wil::scope_exit([&]() noexcept {
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    });

    for (;;) {
        const LONGLONG now = MFGetSystemTime();
        Token token;
        DWORD waitMs = INFINITE;
        bool hasPending = false;

        if (TakeDueRequest(now, token, waitMs, hasPending)) {
            const HRESULT hr = Deliver(token.get(), now);
            if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
                Requeue(std::move(token));
                waitMs = kAllocatorEmptyRetryMs;
            } else {
                if (FAILED(hr) && !reportedDeliveryFailure_) {
                    trace::Writef(trace::Level::Warning, L"Sample delivery failed: 0x{:08X}", static_cast<std::uint32_t>(hr));
                    reportedDeliveryFailure_ = true;
                }
                continue;  // re-evaluate immediately: another request may already be due
            }
        }

        // Frame arrivals only matter while a request is waiting for one.
        const HANDLE waits[] = {stopEvent_.get(), wakeEvent_.get(), receiver_.FrameAvailableEvent()};
        const DWORD count = hasPending ? 3 : 2;
        if (WaitForMultipleObjects(count, waits, FALSE, waitMs) == WAIT_OBJECT_0) return;
    }
}

const std::uint8_t* FrameDelivery::CurrentFrame() noexcept {
    const um::ReceivedFrame& frame = receiver_.AcquireLatest();
    if (receiver_.IsConnected() && frame.valid) {
        placeholderIndex_ = 0;
        return frame.nv12.data();
    }
    // No hub: show our own "no signal" picture rather than a frozen last frame.
    placeholder_.Render(placeholderIndex_++, placeholderFrame_.data());
    return placeholderFrame_.data();
}

HRESULT FrameDelivery::Deliver(IUnknown* token, LONGLONG now) noexcept {
    wil::com_ptr_nothrow<IMFSample> sample;
    RETURN_IF_FAILED_EXPECTED(config_.allocator->AllocateSample(&sample));

    RETURN_IF_FAILED(writer_.Write(sample.get(), CurrentFrame()));
    RETURN_IF_FAILED(sample->SetSampleTime(now));
    RETURN_IF_FAILED(sample->SetSampleDuration(frameInterval_));
    if (token != nullptr) {
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    }

    lastDelivery_ = now;
    reportedDeliveryFailure_ = false;
    RETURN_IF_FAILED_EXPECTED(config_.eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get()));
    return S_OK;
}

}  // namespace mwb::vcam
