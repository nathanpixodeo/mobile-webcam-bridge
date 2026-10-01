#include "video/OutputPolicy.h"

namespace mwb::native {

std::string_view ToString(PlaceholderKind kind) noexcept {
    switch (kind) {
        case PlaceholderKind::NoDevice: return "no-device";
        case PlaceholderKind::AppClosed: return "app-closed";
        case PlaceholderKind::Paused: return "paused";
        case PlaceholderKind::Background: return "background";
        case PlaceholderKind::Stopped: return "stopped";
    }
    return "stopped";
}

std::optional<PlaceholderKind> ParsePlaceholderKind(std::string_view text) noexcept {
    if (text == "no-device") return PlaceholderKind::NoDevice;
    if (text == "app-closed") return PlaceholderKind::AppClosed;
    if (text == "paused") return PlaceholderKind::Paused;
    if (text == "background") return PlaceholderKind::Background;
    if (text == "stopped") return PlaceholderKind::Stopped;
    return std::nullopt;
}

OutputDecision OutputPolicy::Decide(std::uint64_t nowMs) const noexcept {
    if (commanded_) return OutputDecision{OutputMode::Placeholder, *commanded_};
    const bool fresh = lastIngestMs_ && nowMs >= *lastIngestMs_ && nowMs - *lastIngestMs_ <= idleTimeoutMs_;
    if (fresh) return OutputDecision{OutputMode::Live, PlaceholderKind::Stopped};
    return OutputDecision{OutputMode::Placeholder, PlaceholderKind::Stopped};
}

}  // namespace mwb::native
