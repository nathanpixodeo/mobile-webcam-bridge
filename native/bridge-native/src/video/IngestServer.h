// Private ingest pipe: ffmpeg (one writer at a time) writes raw NV12; complete frames are handed
// to the hub. A partial frame left by a disconnecting writer is discarded.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <wil/resource.h>

#include <mwb/FrameProtocol.h>

#include "platform/Security.h"
#include "video/FramePool.h"

namespace mwb::native {

class IngestServer {
public:
    struct Callbacks {
        std::function<void(bool connected)> onConnection;
        // `frame` is tightly packed NV12 of `size`.
        std::function<void(FrameBytes&& frame, mwb::frame::FrameSize size)> onFrame;
    };

    IngestServer(std::wstring pipePath, const std::wstring& sddl, mwb::frame::FrameSize frameSize,
                 std::shared_ptr<FramePool> pool, Callbacks callbacks);
    ~IngestServer();
    IngestServer(const IngestServer&) = delete;
    IngestServer& operator=(const IngestServer&) = delete;

    // Creates the pipe (throws CommandError(PipeInUse) if the name is taken) and starts reading.
    void Start();
    void Stop() noexcept;

    // Switches to frames of `size`: drops the current writer and any partial frame. Returns once
    // the ingest thread applied the size, or false after `timeoutMs`. One caller at a time.
    [[nodiscard]] bool SetFrameSize(mwb::frame::FrameSize size, DWORD timeoutMs);

private:
    void Run() noexcept;
    // Applies a pending SetFrameSize request; true when there was one.
    bool ApplyPendingSize();

    std::wstring pipePath_;
    SecurityAttributes security_;
    std::shared_ptr<FramePool> pool_;
    Callbacks callbacks_;
    wil::unique_handle pipe_;
    // Interrupts any wait of the ingest thread: set by Stop() and by SetFrameSize().
    wil::unique_event interrupt_;
    wil::unique_event sizeApplied_;  // auto-reset
    std::atomic<bool> stopping_{false};
    std::thread thread_;

    std::mutex sizeMutex_;
    std::optional<mwb::frame::FrameSize> pendingSize_;  // guarded by sizeMutex_
    mwb::frame::FrameSize frameSize_;                    // ingest thread only (after Start)
};

}  // namespace mwb::native
