#include "video/IngestServer.h"

#include "platform/NamedPipe.h"
#include "video/FrameAssembler.h"

namespace mwb::native {

IngestServer::IngestServer(std::wstring pipePath, const std::wstring& sddl, std::size_t frameBytes,
                           std::shared_ptr<FramePool> pool, Callbacks callbacks)
    : pipePath_(std::move(pipePath)),
      security_(sddl),
      frameBytes_(frameBytes),
      pool_(std::move(pool)),
      callbacks_(std::move(callbacks)) {
    stop_.create(wil::EventOptions::ManualReset);
}

IngestServer::~IngestServer() { Stop(); }

void IngestServer::Start() {
    PipeServerOptions options;
    options.openMode = PIPE_ACCESS_INBOUND;
    options.firstInstance = true;
    options.maxInstances = 1;  // one writer at a time
    options.outBufferBytes = 4 * 1024;
    options.inBufferBytes = 1024 * 1024;
    pipe_ = CreatePipeServer(pipePath_, options, security_.Get());
    thread_ = std::thread([this] { Run(); });
}

void IngestServer::Stop() noexcept {
    stop_.SetEvent();
    if (thread_.joinable()) thread_.join();
}

void IngestServer::Run() noexcept {
    try {
        FrameAssembler assembler(frameBytes_, [this] { return pool_->Acquire(); });
        OverlappedOperation operation;

        while (true) {
            const IoResult connect = AwaitClient(pipe_.get(), operation, stop_.get());
            if (connect.status == IoStatus::Stopped) break;
            if (connect.status != IoStatus::Completed) {
                ::DisconnectNamedPipe(pipe_.get());
                if (::WaitForSingleObject(stop_.get(), 200) == WAIT_OBJECT_0) break;
                continue;
            }

            callbacks_.onConnection(true);
            assembler.Reset();
            while (true) {
                const std::span<std::uint8_t> region = assembler.WritableRegion();
                const IoResult read = ReadSome(pipe_.get(), operation, region.data(), static_cast<DWORD>(region.size()),
                                               stop_.get());
                if (read.status != IoStatus::Completed) break;  // writer gone or stopping
                if (std::optional<FrameBytes> frame = assembler.Commit(read.bytes)) callbacks_.onFrame(std::move(*frame));
            }
            ::DisconnectNamedPipe(pipe_.get());
            callbacks_.onConnection(false);
            if (::WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0) break;
        }
    } catch (...) {
        // Unexpected failure (out of memory); the hub keeps serving placeholders.
    }
}

}  // namespace mwb::native
