// Public frame pipe: accepts camera components (vcam-mf.dll in Frame Server, vcam-dshow.dll in
// apps) and fans frames out to them, each through its own latest-frame-wins slot.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <wil/resource.h>

#include "platform/Security.h"
#include "video/ConsumerSlot.h"
#include "video/HubTypes.h"

namespace mwb::native {

class OverlappedOperation;

// One connected consumer, served by its own thread. A pending one-byte read on the server end
// completes when the client disconnects (clients open read-only and never write), so departures
// are noticed immediately even when no frame is in flight.
class ConsumerConnection {
public:
    ConsumerConnection(wil::unique_handle pipe, HubCounters& counters, HANDLE exitedEvent);
    ~ConsumerConnection();
    ConsumerConnection(const ConsumerConnection&) = delete;
    ConsumerConnection& operator=(const ConsumerConnection&) = delete;

    void Start();
    void Offer(const OutgoingFrame& frame);
    void RequestStop() noexcept;
    [[nodiscard]] bool Finished() const noexcept { return finished_.load(); }

private:
    void Run() noexcept;
    [[nodiscard]] std::optional<OutgoingFrame> TakePending();
    [[nodiscard]] bool Send(const OutgoingFrame& frame, OverlappedOperation& write);

    wil::unique_handle pipe_;
    HubCounters& counters_;
    HANDLE exitedEvent_;
    wil::unique_event wake_;
    wil::unique_event stop_;
    std::mutex mutex_;
    ConsumerSlot<OutgoingFrame> slot_;
    std::atomic<bool> finished_{false};
    bool sentAny_ = false;
    std::thread thread_;
};

class ConsumerServer {
public:
    using LatestFrameProvider = std::function<std::optional<OutgoingFrame>()>;
    using CountListener = std::function<void(std::size_t)>;

    ConsumerServer(std::wstring pipePath, const std::wstring& sddl, HubCounters& counters, LatestFrameProvider latestFrame,
                   CountListener onCountChanged);
    ~ConsumerServer();
    ConsumerServer(const ConsumerServer&) = delete;
    ConsumerServer& operator=(const ConsumerServer&) = delete;

    // Creates the first pipe instance (FILE_FLAG_FIRST_PIPE_INSTANCE) and starts accepting.
    // Throws CommandError(PipeInUse) when another process owns the name.
    void Start();
    void Stop() noexcept;

    void Broadcast(const OutgoingFrame& frame);
    [[nodiscard]] std::size_t Count() const;

private:
    static constexpr std::size_t kMaxConsumers = 16;

    void AcceptLoop() noexcept;
    void OnClientConnected();
    void Reap();
    [[nodiscard]] wil::unique_handle CreateInstance(bool first);

    std::wstring pipePath_;
    SecurityAttributes security_;
    HubCounters& counters_;
    LatestFrameProvider latestFrame_;
    CountListener onCountChanged_;
    wil::unique_handle listening_;
    wil::unique_event stop_;
    wil::unique_event consumerExited_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ConsumerConnection>> consumers_;
    std::thread thread_;
};

}  // namespace mwb::native
