#include "video/ConsumerServer.h"

#include <algorithm>
#include <iterator>

#include "platform/NamedPipe.h"

namespace mwb::native {

// ---------------------------------------------------------------------------------------------
// ConsumerConnection
// ---------------------------------------------------------------------------------------------

ConsumerConnection::ConsumerConnection(wil::unique_handle pipe, HubCounters& counters, const mwb::frame::ModeCap& cap,
                                       HANDLE exitedEvent, SubscribedListener onSubscribed)
    : pipe_(std::move(pipe)),
      counters_(counters),
      cap_(cap),
      exitedEvent_(exitedEvent),
      onSubscribed_(std::move(onSubscribed)) {
    wake_.create(wil::EventOptions::None);  // auto-reset
    stop_.create(wil::EventOptions::ManualReset);
}

ConsumerConnection::~ConsumerConnection() {
    RequestStop();
    if (thread_.joinable()) thread_.join();
}

void ConsumerConnection::Start() { thread_ = std::thread([this] { Run(); }); }

void ConsumerConnection::Offer(const OutgoingFrame& frame) {
    bool replaced = false;
    {
        const std::lock_guard lock(mutex_);
        replaced = slot_.Offer(frame);
    }
    if (replaced && subscribed_.load()) ++counters_.consumerDrops;
    wake_.SetEvent();
}

void ConsumerConnection::RequestStop() noexcept { stop_.SetEvent(); }

std::optional<mwb::frame::VideoMode> ConsumerConnection::Mode() const noexcept {
    if (!subscribed_.load(std::memory_order_acquire)) return std::nullopt;
    return mode_;
}

std::optional<OutgoingFrame> ConsumerConnection::TakePending() {
    const std::lock_guard lock(mutex_);
    return slot_.Take();
}

const FrameBytes& ConsumerConnection::PayloadFor(const OutgoingFrame& frame) {
    if (frame.width == mode_.width && frame.height == mode_.height) return *frame.payload;
    if (!scaler_ || scaler_->SourceWidth() != frame.width || scaler_->SourceHeight() != frame.height) {
        scaler_ = std::make_unique<mwb::um::color::Nv12Scaler>(frame.width, frame.height, mode_.width, mode_.height);
    }
    scaled_.resize(mwb::frame::Nv12FrameBytes(mode_.width, mode_.height));
    scaler_->Scale(mwb::um::color::PackedNv12(frame.payload->data(), frame.width, frame.height), scaled_.data());
    return scaled_;
}

bool ConsumerConnection::Send(const OutgoingFrame& frame, OverlappedOperation& write) {
    // Frames come from the hub itself; a size that does not match its bytes is a bug, never sent.
    if (frame.payload == nullptr || frame.payload->size() != mwb::frame::Nv12FrameBytes(frame.width, frame.height)) {
        return true;
    }
    const FrameBytes& payload = PayloadFor(frame);

    std::uint32_t flags = frame.flags;
    if (!sentAny_) flags |= mwb::frame::kFlagFormatChanged;
    const mwb::frame::FrameHeader header =
        mwb::frame::MakeHeader(mode_.width, mode_.height, flags, frame.seq, frame.producerQpc100ns);

    if (WriteAll(pipe_.get(), write, &header, sizeof(header), stop_.get()).status != IoStatus::Completed) return false;
    const auto size = static_cast<DWORD>(payload.size());
    if (WriteAll(pipe_.get(), write, payload.data(), size, stop_.get()).status != IoStatus::Completed) return false;

    sentAny_ = true;
    ++counters_.framesOut;
    return true;
}

bool ConsumerConnection::Subscribe() {
    OverlappedOperation read;
    mwb::frame::SubscribeRequest request{};
    const IoResult result = ReadExactly(pipe_.get(), read, &request, sizeof(request), stop_.get(), kSubscribeTimeoutMs);
    if (result.status != IoStatus::Completed) return false;
    if (mwb::frame::ValidateSubscribe(request, cap_) != mwb::frame::SubscribeError::None) return false;
    mode_ = mwb::frame::ModeOf(request);
    subscribed_.store(true, std::memory_order_release);
    return true;
}

void ConsumerConnection::Serve() {
    OverlappedOperation disconnect;
    std::uint8_t sink = 0;
    const BOOL readStarted = ::ReadFile(pipe_.get(), &sink, 1, nullptr, disconnect.Get());
    if (readStarted || ::GetLastError() != ERROR_IO_PENDING) return;  // a second write breaks the protocol

    OverlappedOperation write;
    bool connected = true;
    while (connected) {
        const HANDLE waits[3] = {stop_.get(), disconnect.Event(), wake_.get()};
        if (::WaitForMultipleObjects(3, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 2) break;
        while (connected) {
            std::optional<OutgoingFrame> frame = TakePending();
            if (!frame) break;
            connected = Send(*frame, write);
        }
    }
    ::CancelIoEx(pipe_.get(), disconnect.Get());
    DWORD ignored = 0;
    ::GetOverlappedResult(pipe_.get(), disconnect.Get(), &ignored, TRUE);
}

void ConsumerConnection::Run() noexcept {
    try {
        if (Subscribe()) {
            onSubscribed_();
            Serve();  // frames offered while subscribing are waiting in the slot (wake_ is set)
        }
    } catch (...) {
        // A consumer failing must never take the hub down; it is simply dropped.
    }
    finished_.store(true);
    ::SetEvent(exitedEvent_);
}

// ---------------------------------------------------------------------------------------------
// ConsumerServer
// ---------------------------------------------------------------------------------------------

ConsumerServer::ConsumerServer(std::wstring pipePath, const std::wstring& sddl, HubCounters& counters,
                               const mwb::frame::ModeCap& cap, LatestFrameProvider latestFrame,
                               ConsumersListener onConsumersChanged)
    : pipePath_(std::move(pipePath)),
      security_(sddl),
      counters_(counters),
      cap_(cap),
      latestFrame_(std::move(latestFrame)),
      onConsumersChanged_(std::move(onConsumersChanged)) {
    stop_.create(wil::EventOptions::ManualReset);
    consumerExited_.create(wil::EventOptions::None);
}

ConsumerServer::~ConsumerServer() { Stop(); }

wil::unique_handle ConsumerServer::CreateInstance(bool first) {
    PipeServerOptions options;
    options.openMode = PIPE_ACCESS_DUPLEX;  // the client writes its subscription
    options.firstInstance = first;
    options.maxInstances = PIPE_UNLIMITED_INSTANCES;
    options.outBufferBytes = 1024 * 1024;
    options.inBufferBytes = 4 * 1024;
    return CreatePipeServer(pipePath_, options, security_.Get());
}

void ConsumerServer::Start() {
    listening_ = CreateInstance(true);
    thread_ = std::thread([this] { AcceptLoop(); });
}

void ConsumerServer::Stop() noexcept {
    stop_.SetEvent();
    if (thread_.joinable()) thread_.join();
    std::vector<std::unique_ptr<ConsumerConnection>> remaining;
    {
        const std::lock_guard lock(mutex_);
        remaining.swap(consumers_);
    }
    remaining.clear();  // each destructor stops and joins its thread
}

void ConsumerServer::Broadcast(const OutgoingFrame& frame) {
    const std::lock_guard lock(mutex_);
    for (const std::unique_ptr<ConsumerConnection>& consumer : consumers_) {
        if (!consumer->Finished()) consumer->Offer(frame);
    }
}

std::size_t ConsumerServer::Count() const { return Modes().size(); }

std::vector<mwb::frame::VideoMode> ConsumerServer::Modes() const {
    std::vector<mwb::frame::VideoMode> modes;
    const std::lock_guard lock(mutex_);
    for (const std::unique_ptr<ConsumerConnection>& consumer : consumers_) {
        if (consumer->Finished()) continue;
        if (const std::optional<mwb::frame::VideoMode> mode = consumer->Mode()) modes.push_back(*mode);
    }
    return modes;
}

void ConsumerServer::NotifyIfChanged() {
    const std::lock_guard notifyLock(notifyMutex_);
    std::vector<mwb::frame::VideoMode> modes = Modes();
    if (modes == reported_) return;
    reported_ = std::move(modes);
    onConsumersChanged_(reported_);
}

void ConsumerServer::OnClientConnected() {
    wil::unique_handle client = std::move(listening_);

    // Latest frame first, outside our lock: the provider takes the hub's lock, and the hub calls
    // Broadcast while holding it (hub lock → server lock is the only allowed order).
    std::optional<OutgoingFrame> latest = latestFrame_();
    auto connection = std::make_unique<ConsumerConnection>(std::move(client), counters_, cap_, consumerExited_.get(),
                                                           [this] { NotifyIfChanged(); });
    if (latest) connection->Offer(*latest);

    const std::lock_guard lock(mutex_);
    if (consumers_.size() < kMaxConnections) {
        connection->Start();
        consumers_.push_back(std::move(connection));
    }
    // A rejected connection is closed when `connection` goes out of scope. The client counts as a
    // consumer only once its subscription is accepted (NotifyIfChanged from its thread).
}

void ConsumerServer::Reap() {
    std::vector<std::unique_ptr<ConsumerConnection>> finished;
    {
        const std::lock_guard lock(mutex_);
        const auto split = std::stable_partition(consumers_.begin(), consumers_.end(),
                                                 [](const auto& consumer) { return !consumer->Finished(); });
        std::move(split, consumers_.end(), std::back_inserter(finished));
        consumers_.erase(split, consumers_.end());
    }
    if (finished.empty()) return;
    finished.clear();  // joins outside the lock
    NotifyIfChanged();
}

void ConsumerServer::AcceptLoop() noexcept {
    OverlappedOperation connect;
    bool connectPending = false;

    while (true) {
        try {
            if (!listening_) {
                listening_ = CreateInstance(false);
            }
            if (!connectPending) {
                connect.Reset();
                const BOOL started = ::ConnectNamedPipe(listening_.get(), connect.Get());
                const DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
                if (started || error == ERROR_PIPE_CONNECTED) {
                    OnClientConnected();
                    continue;
                }
                if (error != ERROR_IO_PENDING) {
                    listening_.reset();  // recreate the instance and try again
                    if (::WaitForSingleObject(stop_.get(), 250) == WAIT_OBJECT_0) break;
                    continue;
                }
                connectPending = true;
            }

            const HANDLE waits[3] = {stop_.get(), connect.Event(), consumerExited_.get()};
            const DWORD wait = ::WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 2) {
                Reap();
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 1) break;  // stop requested (or wait failure)

            connectPending = false;
            DWORD ignored = 0;
            if (::GetOverlappedResult(listening_.get(), connect.Get(), &ignored, FALSE)) {
                OnClientConnected();
            } else {
                ::DisconnectNamedPipe(listening_.get());
            }
        } catch (...) {
            // Creating an instance failed (resource exhaustion); back off and retry.
            connectPending = false;
            listening_.reset();
            if (::WaitForSingleObject(stop_.get(), 500) == WAIT_OBJECT_0) break;
        }
    }

    if (connectPending && listening_) {
        ::CancelIoEx(listening_.get(), connect.Get());
        DWORD ignored = 0;
        ::GetOverlappedResult(listening_.get(), connect.Get(), &ignored, TRUE);
    }
}

}  // namespace mwb::native
