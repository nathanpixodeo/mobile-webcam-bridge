#include "video/ConsumerServer.h"

#include <algorithm>
#include <iterator>

#include "platform/NamedPipe.h"

namespace mwb::native {

// ---------------------------------------------------------------------------------------------
// ConsumerConnection
// ---------------------------------------------------------------------------------------------

ConsumerConnection::ConsumerConnection(wil::unique_handle pipe, HubCounters& counters, HANDLE exitedEvent)
    : pipe_(std::move(pipe)), counters_(counters), exitedEvent_(exitedEvent) {
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
    if (replaced) ++counters_.consumerDrops;
    wake_.SetEvent();
}

void ConsumerConnection::RequestStop() noexcept { stop_.SetEvent(); }

std::optional<OutgoingFrame> ConsumerConnection::TakePending() {
    const std::lock_guard lock(mutex_);
    return slot_.Take();
}

bool ConsumerConnection::Send(const OutgoingFrame& frame, OverlappedOperation& write) {
    mwb::frame::FrameHeader header = frame.header;
    if (!sentAny_) header.flags |= mwb::frame::kFlagFormatChanged;

    if (WriteAll(pipe_.get(), write, &header, sizeof(header), stop_.get()).status != IoStatus::Completed) return false;
    const auto size = static_cast<DWORD>(frame.payload->size());
    if (WriteAll(pipe_.get(), write, frame.payload->data(), size, stop_.get()).status != IoStatus::Completed) return false;

    sentAny_ = true;
    ++counters_.framesOut;
    return true;
}

void ConsumerConnection::Run() noexcept {
    try {
        OverlappedOperation disconnect;
        std::uint8_t sink = 0;
        const BOOL readStarted = ::ReadFile(pipe_.get(), &sink, 1, nullptr, disconnect.Get());
        const bool pending = !readStarted && ::GetLastError() == ERROR_IO_PENDING;

        if (pending) {
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
                               LatestFrameProvider latestFrame, CountListener onCountChanged)
    : pipePath_(std::move(pipePath)),
      security_(sddl),
      counters_(counters),
      latestFrame_(std::move(latestFrame)),
      onCountChanged_(std::move(onCountChanged)) {
    stop_.create(wil::EventOptions::ManualReset);
    consumerExited_.create(wil::EventOptions::None);
}

ConsumerServer::~ConsumerServer() { Stop(); }

wil::unique_handle ConsumerServer::CreateInstance(bool first) {
    PipeServerOptions options;
    options.openMode = PIPE_ACCESS_DUPLEX;  // duplex so the disconnect-detecting read is possible
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

std::size_t ConsumerServer::Count() const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(consumers_.begin(), consumers_.end(),
                                                  [](const auto& consumer) { return !consumer->Finished(); }));
}

void ConsumerServer::OnClientConnected() {
    wil::unique_handle client = std::move(listening_);

    // Latest frame first, outside our lock: the provider takes the hub's lock, and the hub calls
    // Broadcast while holding it (hub lock → server lock is the only allowed order).
    std::optional<OutgoingFrame> latest = latestFrame_();
    auto connection = std::make_unique<ConsumerConnection>(std::move(client), counters_, consumerExited_.get());
    if (latest) connection->Offer(*latest);

    std::size_t count = 0;
    bool accepted = false;
    {
        const std::lock_guard lock(mutex_);
        if (consumers_.size() < kMaxConsumers) {
            connection->Start();
            consumers_.push_back(std::move(connection));
            accepted = true;
        }
        count = consumers_.size();
    }
    if (accepted) onCountChanged_(count);
    // A rejected connection is closed when `connection` goes out of scope.
}

void ConsumerServer::Reap() {
    std::vector<std::unique_ptr<ConsumerConnection>> finished;
    std::size_t count = 0;
    {
        const std::lock_guard lock(mutex_);
        const auto split = std::stable_partition(consumers_.begin(), consumers_.end(),
                                                 [](const auto& consumer) { return !consumer->Finished(); });
        std::move(split, consumers_.end(), std::back_inserter(finished));
        consumers_.erase(split, consumers_.end());
        count = consumers_.size();
    }
    if (finished.empty()) return;
    finished.clear();  // joins outside the lock
    onCountChanged_(count);
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
