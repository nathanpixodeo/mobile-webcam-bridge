// Decides what the public pipe shows: live ingest frames or a placeholder
// (protocol/BRIDGE_NATIVE.md, `video hub` stdin commands). Pure logic over a millisecond clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mwb::native {

enum class PlaceholderKind { NoDevice, AppClosed, Paused, Background, Stopped };

inline constexpr std::size_t kPlaceholderKindCount = 5;

[[nodiscard]] std::string_view ToString(PlaceholderKind kind) noexcept;
[[nodiscard]] std::optional<PlaceholderKind> ParsePlaceholderKind(std::string_view text) noexcept;

enum class OutputMode { Live, Placeholder };

struct OutputDecision {
    OutputMode mode = OutputMode::Placeholder;
    PlaceholderKind placeholder = PlaceholderKind::Stopped;  // meaningful for OutputMode::Placeholder

    friend bool operator==(const OutputDecision&, const OutputDecision&) = default;
};

class OutputPolicy {
public:
    static constexpr std::uint64_t kDefaultIdleTimeoutMs = 300;

    explicit OutputPolicy(std::uint64_t idleTimeoutMs = kDefaultIdleTimeoutMs) noexcept : idleTimeoutMs_(idleTimeoutMs) {}

    // A placeholder kind pins that placeholder; nullopt returns to live frames.
    void Command(std::optional<PlaceholderKind> kind) noexcept { commanded_ = kind; }
    void OnIngestFrame(std::uint64_t nowMs) noexcept { lastIngestMs_ = nowMs; }

    [[nodiscard]] OutputDecision Decide(std::uint64_t nowMs) const noexcept;

private:
    std::uint64_t idleTimeoutMs_;
    std::optional<PlaceholderKind> commanded_;
    std::optional<std::uint64_t> lastIngestMs_;
};

}  // namespace mwb::native
