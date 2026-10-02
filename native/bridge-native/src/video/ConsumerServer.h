// Public frame pipe: accepts camera components (vcam-mf.dll in Frame Server, vcam-dshow.dll in
// apps), reads each one's subscription (protocol/FRAME_PIPE.md §3.1) and fans frames out to them,
// each through its own latest-frame-wins slot, scaled to the size it subscribed to.
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

#include <mwb/FrameProtocol.h>
#include <mwb/um/Nv12Scaler.h>

#include "platform/Security.h"
#include "video/ConsumerSlot.h"
#include "video/HubTypes.h"

namespace mwb::native {

class OverlappedOperation;

// One connected client, served by its own thread. It first reads the subscription; once accepted
// the client is a consumer. A pending one-byte read on the server end then completes when the
// client disconnects (or, against the protocol, writes again), so departures are noticed
// immediately even when no frame is in flight.
class ConsumerConnection {
public:
    using SubscribedListener = std::function<void()>;

    ConsumerConnection(wil::unique_handle pipe, HubCounters& counters, const mwb::frame::ModeCap& cap, HANDLE exitedEvent,
                       SubscribedListener onSubscribed);
    ~ConsumerConnection();
    ConsumerConnection(const ConsumerConnection&) = delete;
    ConsumerConnection& operator=(const ConsumerConnection&) = delete;

    void Start();
    void Offer(const OutgoingFrame& frame);
    void RequestStop() noexcept;
    [[nodiscard]] bool Finished() const noexcept { return finished_.load(); }
    // The subscribed mode; nullopt until the subscription has been accepted.
    [[nodiscard]] std::optional<mwb::frame::VideoMode> Mode() const noexcept;

private:
    static constexpr DWORD kSubscribeTimeoutMs = 2000;

    void Run() noexcept;
    [[nodiscard]] bool Subscribe();
    void Serve();
    [[nodiscard]] std::optional<OutgoingFrame> TakePending();
    [[nodiscard]] bool Send(const OutgoingFrame& frame, OverlappedOperation& write);
    // The payload at the subscribed size: the frame's own bytes or a scaled copy in scaled_.
    [[nodiscard]] const FrameBytes& PayloadFor(const OutgoingFrame& frame);

    wil::unique_handle pipe_;
    HubCounters& counters_;
    mwb::frame::ModeCap cap_;
    HANDLE exitedEvent_;
    SubscribedListener onSubscribed_;
    wil::unique_event wake_;
    wil::unique_event stop_;
    std::mutex mutex_;
    ConsumerSlot<OutgoingFrame> slot_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> subscribed_{false};
    mwb::frame::VideoMode mode_{};  // written once before subscribed_ is set
    bool sentAny_ = false;
    std::unique_ptr<mwb::um::color::Nv12Scaler> scaler_;  // connection thread only
    FrameBytes scaled_;
    std::thread thread_;
};

class ConsumerServer {
public:
    using LatestFrameProvider = std::function<std::optional<OutgoingFrame>()>;
    // The subscribed consumers' modes, in connection order; called on every change.
    using ConsumersListener = std::function<void(const std::vector<mwb::frame::VideoMode>&)>;

    ConsumerServer(std::wstring pipePath, const std::wstring& sddl, HubCounters& counters, const mwb::frame::ModeCap& cap,
                   LatestFrameProvider latestFrame, ConsumersListener onConsumersChanged);
    ~ConsumerServer();
    ConsumerServer(const ConsumerServer&) = delete;
    ConsumerServer& operator=(const ConsumerServer&) = delete;

    // Creates the first pipe instance (FILE_FLAG_FIRST_PIPE_INSTANCE) and starts accepting.
    // Throws CommandError(PipeInUse) when another process owns the name.
    void Start();
    void Stop() noexcept;

    void Broadcast(const OutgoingFrame& frame);
    // Subscribed consumers that are still connected.
    [[nodiscard]] std::size_t Count() const;
    [[nodiscard]] std::vector<mwb::frame::VideoMode> Modes() const;

private:
    static constexpr std::size_t kMaxConnections = 16;

    void AcceptLoop() noexcept;
    void OnClientConnected();
    void Reap();
    // Reports the consumers' modes when they differ from the last report.
    void NotifyIfChanged();
    [[nodiscard]] wil::unique_handle CreateInstance(bool first);

    std::wstring pipePath_;
    SecurityAttributes security_;
    HubCounters& counters_;
    mwb::frame::ModeCap cap_;
    LatestFrameProvider latestFrame_;
    ConsumersListener onConsumersChanged_;
    wil::unique_handle listening_;
    wil::unique_event stop_;
    wil::unique_event consumerExited_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ConsumerConnection>> consumers_;
    // Serialises reports so they reach the listener in order; taken before mutex_.
    std::mutex notifyMutex_;
    std::vector<mwb::frame::VideoMode> reported_;
    std::thread thread_;
};

}  // namespace mwb::native
