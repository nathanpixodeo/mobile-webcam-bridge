// Private ingest pipe: ffmpeg (one writer at a time) writes raw NV12; complete frames are handed
// to the hub. A partial frame left by a disconnecting writer is discarded.
#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <wil/resource.h>

#include "platform/Security.h"
#include "video/FramePool.h"

namespace mwb::native {

class IngestServer {
public:
    struct Callbacks {
        std::function<void(bool connected)> onConnection;
        std::function<void(FrameBytes&& frame)> onFrame;
    };

    IngestServer(std::wstring pipePath, const std::wstring& sddl, std::size_t frameBytes, std::shared_ptr<FramePool> pool,
                 Callbacks callbacks);
    ~IngestServer();
    IngestServer(const IngestServer&) = delete;
    IngestServer& operator=(const IngestServer&) = delete;

    // Creates the pipe (throws CommandError(PipeInUse) if the name is taken) and starts reading.
    void Start();
    void Stop() noexcept;

private:
    void Run() noexcept;

    std::wstring pipePath_;
    SecurityAttributes security_;
    std::size_t frameBytes_;
    std::shared_ptr<FramePool> pool_;
    Callbacks callbacks_;
    wil::unique_handle pipe_;
    wil::unique_event stop_;
    std::thread thread_;
};

}  // namespace mwb::native
