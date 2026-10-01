// `bridge-native video hub`: owns the ingest and public pipes, picks live or placeholder output,
// reports consumers/stats on stdout and follows stdin commands until stdin closes.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include <wil/resource.h>

#include "core/Errors.h"
#include "video/ConsumerServer.h"
#include "video/HubCommand.h"
#include "video/HubSettings.h"
#include "video/IngestServer.h"
#include "video/OutputPolicy.h"
#include "video/PlaceholderStore.h"

namespace mwb::native {

class Console;

class VideoHub {
public:
    VideoHub(Console& console, HubSettings settings);
    ~VideoHub();
    VideoHub(const VideoHub&) = delete;
    VideoHub& operator=(const VideoHub&) = delete;

    // Runs until stdin reaches EOF. Pipe creation failures throw CommandError.
    [[nodiscard]] ExitCode Run();

private:
    void OnIngestFrame(FrameBytes&& frame);
    void OnIngestConnection(bool connected);
    void OnConsumerCount(std::size_t count);
    void OnCommandLine(std::string_view line, bool overflow);
    void Apply(const LoadPlaceholderCommand& command);
    void Apply(const ShowPlaceholderCommand& command);
    void ReadCommandsUntilEof();
    void Tick();

    // Callers hold mutex_.
    void RefreshOutputLocked(std::uint64_t nowMs, bool force);
    void PublishLocked(SharedFrameBytes payload, std::uint32_t flags);

    [[nodiscard]] std::optional<OutgoingFrame> LatestFrame() const;
    void EmitReady();
    void EmitStats();
    void EmitError(const CommandError& error, bool fatal);

    Console& console_;
    HubSettings settings_;
    std::size_t frameBytes_;
    HubCounters counters_;
    std::shared_ptr<FramePool> pool_;

    mutable std::mutex mutex_;
    OutputPolicy policy_;
    PlaceholderStore placeholders_;
    OutputDecision current_;
    bool outputPublished_ = false;
    std::optional<OutgoingFrame> latest_;
    std::uint64_t seq_ = 0;

    std::unique_ptr<ConsumerServer> consumers_;
    std::unique_ptr<IngestServer> ingest_;
    wil::unique_event shutdown_;
};

}  // namespace mwb::native
