#include "video/IngestServer.h"

#include "platform/NamedPipe.h"
#include "video/FrameAssembler.h"

namespace mwb::native {

IngestServer::IngestServer(std::wstring pipePath, const std::wstring& sddl, mwb::frame::FrameSize frameSize,
                           std::shared_ptr<FramePool> pool, Callbacks callbacks)
    : pipePath_(std::move(pipePath)),
      security_(sddl),
      pool_(std::move(pool)),
      callbacks_(std::move(callbacks)),
      frameSize_(frameSize) {
    interrupt_.create(wil::EventOptions::ManualReset);
    sizeApplied_.create(wil::EventOptions::None);
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
    stopping_.store(true);
    interrupt_.SetEvent();
    if (thread_.joinable()) thread_.join();
}

bool IngestServer::SetFrameSize(mwb::frame::FrameSize size, DWORD timeoutMs) {
    sizeApplied_.ResetEvent();
    {
        const std::lock_guard lock(sizeMutex_);
        pendingSize_ = size;
    }
    interrupt_.SetEvent();
    return ::WaitForSingleObject(sizeApplied_.get(), timeoutMs) == WAIT_OBJECT_0;
}

bool IngestServer::ApplyPendingSize() {
    std::optional<mwb::frame::FrameSize> size;
    {
        const std::lock_guard lock(sizeMutex_);
        size.swap(pendingSize_);
    }
    if (!size) return false;
    frameSize_ = *size;
    pool_->SetFrameBytes(mwb::frame::Nv12FrameBytes(size->width, size->height));
    sizeApplied_.SetEvent();
    return true;
}

void IngestServer::Run() noexcept {
    try {
        const auto makeAssembler = [this] {
            return FrameAssembler(mwb::frame::Nv12FrameBytes(frameSize_.width, frameSize_.height),
                                  [this] { return pool_->Acquire(); });
        };
        FrameAssembler assembler = makeAssembler();
        OverlappedOperation operation;

        // The interrupt event doubles as the "stop" handle of every wait: Stopped means either a
        // real stop or a size change, told apart by stopping_.
        const auto onInterrupt = [&] {
            if (stopping_.load()) return false;
            interrupt_.ResetEvent();
            if (ApplyPendingSize()) assembler = makeAssembler();
            return true;
        };

        while (!stopping_.load()) {
            const IoResult connect = AwaitClient(pipe_.get(), operation, interrupt_.get());
            if (connect.status == IoStatus::Stopped) {
                ::DisconnectNamedPipe(pipe_.get());
                if (!onInterrupt()) break;
                continue;
            }
            if (connect.status != IoStatus::Completed) {
                ::DisconnectNamedPipe(pipe_.get());
                if (::WaitForSingleObject(interrupt_.get(), 200) == WAIT_OBJECT_0 && !onInterrupt()) break;
                continue;
            }

            callbacks_.onConnection(true);
            assembler.Reset();
            bool interrupted = false;
            while (true) {
                const std::span<std::uint8_t> region = assembler.WritableRegion();
                const IoResult read = ReadSome(pipe_.get(), operation, region.data(), static_cast<DWORD>(region.size()),
                                               interrupt_.get());
                if (read.status == IoStatus::Stopped) interrupted = true;
                if (read.status != IoStatus::Completed) break;  // writer gone, stopping or resizing
                if (std::optional<FrameBytes> frame = assembler.Commit(read.bytes)) {
                    callbacks_.onFrame(std::move(*frame), frameSize_);
                }
            }
            ::DisconnectNamedPipe(pipe_.get());
            callbacks_.onConnection(false);
            if (interrupted && !onInterrupt()) break;
        }
    } catch (...) {
        // Unexpected failure (out of memory); the hub keeps serving placeholders.
    }
}

}  // namespace mwb::native
