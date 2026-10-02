#include "video/VideoHub.h"

#include <windows.h>

#include <thread>
#include <variant>

#include "core/Json.h"
#include "core/Output.h"
#include "core/Strings.h"
#include "platform/NamedPipe.h"
#include "platform/Security.h"
#include "platform/Stdin.h"
#include "video/ModeJson.h"

namespace mwb::native {

namespace {

constexpr std::size_t kPooledFrames = 8;
constexpr std::size_t kMaxCommandLineBytes = 64 * 1024;
constexpr DWORD kTickMs = 50;
constexpr std::uint64_t kStatsIntervalMs = 5000;
constexpr DWORD kIngestResizeTimeoutMs = 2000;

std::uint64_t NowMs() noexcept { return ::GetTickCount64(); }

// QueryPerformanceCounter in 100 ns units, the time base MFGetSystemTime() also uses.
std::uint64_t NowQpc100ns() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    ::QueryPerformanceCounter(&counter);
    ::QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    const auto perSecond = static_cast<std::uint64_t>(frequency.QuadPart);
    return (ticks / perSecond) * 10'000'000ull + (ticks % perSecond) * 10'000'000ull / perSecond;
}

}  // namespace

VideoHub::VideoHub(Console& console, HubSettings settings)
    : console_(console),
      settings_(std::move(settings)),
      pool_(FramePool::Create(mwb::frame::Nv12FrameBytes(settings_.defaultMode.width, settings_.defaultMode.height),
                              kPooledFrames)),
      placeholders_(settings_.defaultMode.width, settings_.defaultMode.height) {
    shutdown_.create(wil::EventOptions::ManualReset);
}

VideoHub::~VideoHub() {
    if (ingest_) ingest_->Stop();
    if (consumers_) consumers_->Stop();
}

ExitCode VideoHub::Run() {
    const std::wstring userSid = CurrentUserSid();
    consumers_ = std::make_unique<ConsumerServer>(
        PipePath(settings_.publicPipeName), PublicFramePipeSddl(userSid), counters_, settings_.cap,
        [this] { return LatestFrame(); },
        [this](const std::vector<mwb::frame::VideoMode>& modes) { OnConsumers(modes); });
    ingest_ = std::make_unique<IngestServer>(
        settings_.ingestPipePath, PrivatePipeSddl(userSid),
        mwb::frame::FrameSize{settings_.defaultMode.width, settings_.defaultMode.height}, pool_,
        IngestServer::Callbacks{
            [this](bool connected) { OnIngestConnection(connected); },
            [this](FrameBytes&& frame, mwb::frame::FrameSize size) { OnIngestFrame(std::move(frame), size); }});

    consumers_->Start();
    ingest_->Start();
    {
        const std::lock_guard lock(mutex_);
        RefreshOutputLocked(NowMs(), true);  // consumers see a placeholder right away
    }
    EmitReady();

    std::thread commands([this] { ReadCommandsUntilEof(); });
    std::uint64_t lastStats = NowMs();
    while (::WaitForSingleObject(shutdown_.get(), kTickMs) == WAIT_TIMEOUT) {
        Tick();
        if (NowMs() - lastStats >= kStatsIntervalMs) {
            EmitStats();
            lastStats = NowMs();
        }
    }

    ingest_->Stop();
    consumers_->Stop();
    if (commands.joinable()) {
        ::CancelSynchronousIo(commands.native_handle());  // no-op when it already saw EOF
        commands.join();
    }
    return ExitCode::Success;
}

void VideoHub::ReadCommandsUntilEof() {
    StdinReader reader;
    LineSplitter splitter(kMaxCommandLineBytes);
    char buffer[4096];
    while (true) {
        const std::size_t read = reader.Read(buffer, sizeof(buffer));
        if (read == 0) break;
        splitter.Push(std::string_view(buffer, read),
                      [this](std::string_view line, bool overflow) { OnCommandLine(line, overflow); });
    }
    shutdown_.SetEvent();
}

void VideoHub::OnCommandLine(std::string_view line, bool overflow) {
    if (overflow) {
        EmitError(CommandError(ErrorCode::Usage, "command line too long"), false);
        return;
    }
    if (line.find_first_not_of(" \t") == std::string_view::npos) return;  // blank line
    try {
        const HubCommand command = ParseHubCommand(line);
        std::visit([this](const auto& typed) { Apply(typed); }, command);
    } catch (const HubCommandError& error) {
        EmitError(CommandError(ErrorCode::Usage, error.what()), false);
    } catch (...) {
        EmitError(CurrentExceptionToCommandError(), false);
    }
}

void VideoHub::Apply(const LoadPlaceholderCommand& command) {
    // File I/O outside the lock.
    FrameBytes frame = ReadPlaceholderFile(command.path, mwb::frame::Nv12FrameBytes(command.width, command.height));
    const std::lock_guard lock(mutex_);
    placeholders_.Set(command.kind, std::move(frame), command.width, command.height);
    const bool showing = current_.mode == OutputMode::Placeholder && current_.placeholder == command.kind;
    if (showing) RefreshOutputLocked(NowMs(), true);  // show the new image immediately
}

void VideoHub::Apply(const ShowPlaceholderCommand& command) {
    const std::lock_guard lock(mutex_);
    policy_.Command(command.kind);
    RefreshOutputLocked(NowMs(), false);
}

void VideoHub::Apply(const SetIngestSizeCommand& command) {
    // The parser validated the size (frame::IsIngestSize); the cap only limits consumers.
    const mwb::frame::FrameSize size{command.width, command.height};
    if (!ingest_->SetFrameSize(size, kIngestResizeTimeoutMs)) {
        throw CommandError(ErrorCode::IoError, "Timed out switching the ingest size");
    }
    EmitIngestMode(size);
}

void VideoHub::Tick() {
    const std::lock_guard lock(mutex_);
    RefreshOutputLocked(NowMs(), false);
}

void VideoHub::OnIngestFrame(FrameBytes&& frame, mwb::frame::FrameSize size) {
    ++counters_.framesIn;
    const std::lock_guard lock(mutex_);
    const std::uint64_t now = NowMs();
    policy_.OnIngestFrame(now);
    const OutputDecision decision = policy_.Decide(now);
    if (decision.mode != OutputMode::Live) {
        pool_->Release(std::move(frame));  // a pinned placeholder wins over live frames
        RefreshOutputLocked(now, false);
        return;
    }
    current_ = decision;
    outputPublished_ = true;
    PublishLocked(pool_->Share(std::move(frame)), size, 0);
}

void VideoHub::RefreshOutputLocked(std::uint64_t nowMs, bool force) {
    const OutputDecision decision = policy_.Decide(nowMs);
    if (!force && outputPublished_ && decision == current_) return;
    current_ = decision;
    outputPublished_ = true;
    if (decision.mode == OutputMode::Placeholder) {
        const PlaceholderFrame& placeholder = placeholders_.Get(decision.placeholder);
        PublishLocked(placeholder.bytes, mwb::frame::FrameSize{placeholder.width, placeholder.height},
                      mwb::frame::kFlagPlaceholder);
        ++counters_.placeholderFrames;
    }
    // Live: the next ingest frame is published as it arrives.
}

void VideoHub::PublishLocked(SharedFrameBytes payload, mwb::frame::FrameSize size, std::uint32_t flags) {
    latest_ = OutgoingFrame{std::move(payload), size.width, size.height, flags, seq_++, NowQpc100ns()};
    consumers_->Broadcast(*latest_);
}

std::optional<OutgoingFrame> VideoHub::LatestFrame() const {
    const std::lock_guard lock(mutex_);
    return latest_;
}

void VideoHub::OnIngestConnection(bool connected) {
    JsonWriter writer;
    writer.BeginObject().Field("event", "ingest").Field("connected", connected).EndObject();
    console_.Emit(writer);
}

void VideoHub::OnConsumers(const std::vector<mwb::frame::VideoMode>& modes) {
    JsonWriter writer;
    writer.BeginObject().Field("event", "consumers").Field("count", static_cast<std::uint64_t>(modes.size()));
    writer.Key("modes").BeginArray();
    for (const mwb::frame::VideoMode& mode : modes) WriteMode(writer, mode);
    writer.EndArray().EndObject();
    console_.Emit(writer);
}

void VideoHub::EmitReady() {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "ready")
        .Field("ingestPipe", settings_.ingestPipePath)
        .Field("publicPipe", PipePath(settings_.publicPipeName));
    WriteModeFields(writer, settings_.defaultMode);
    WriteCapFields(writer, settings_.cap);
    writer.EndObject();
    console_.Emit(writer);
}

void VideoHub::EmitIngestMode(mwb::frame::FrameSize size) {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "ingestMode")
        .Field("width", size.width)
        .Field("height", size.height)
        .EndObject();
    console_.Emit(writer);
}

void VideoHub::EmitStats() {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "stats")
        .Field("framesIn", counters_.framesIn.load())
        .Field("framesOut", counters_.framesOut.load())
        .Field("consumerDrops", counters_.consumerDrops.load())
        .Field("placeholderFrames", counters_.placeholderFrames.load())
        .Field("consumers", static_cast<std::uint64_t>(consumers_->Count()))
        .EndObject();
    console_.Emit(writer);
}

void VideoHub::EmitError(const CommandError& error, bool fatal) {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "error")
        .Field("code", ToString(error.Code()))
        .Field("message", error.Message())
        .Field("fatal", fatal)
        .EndObject();
    console_.Emit(writer);
    console_.Warn(error.Message());
}

}  // namespace mwb::native
