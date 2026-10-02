// Frame pipe protocol v2 — the contract between `bridge-native video hub` and the camera
// components (vcam-mf.dll, vcam-dshow.dll). Specification: protocol/FRAME_PIPE.md.
//
// User-mode only. Header-only, no Windows dependencies, so it is unit-testable anywhere.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mwb::frame {

static_assert(std::endian::native == std::endian::little, "Frame pipe protocol is little-endian");

inline constexpr std::uint32_t kMagic = 0x5642574Du;       // bytes 'M' 'W' 'B' 'V'
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::uint16_t kHeaderSize = 48;
inline constexpr std::uint32_t kFourccNv12 = 0x3231564Eu;  // bytes 'N' 'V' '1' '2'

inline constexpr std::uint32_t kFlagPlaceholder = 0x1u;
inline constexpr std::uint32_t kFlagFormatChanged = 0x2u;
inline constexpr std::uint32_t kKnownFlags = kFlagPlaceholder | kFlagFormatChanged;

inline constexpr std::uint32_t kMaxWidth = 3840;
inline constexpr std::uint32_t kMaxHeight = 2160;

inline constexpr wchar_t kPipeNamespace[] = L"\\\\.\\pipe\\";
inline constexpr wchar_t kDefaultPublicPipeName[] = L"mobile-webcam-bridge-video";
inline constexpr wchar_t kIngestPipePrefix[] = L"mobile-webcam-bridge-ingest-";

struct FrameHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t headerSize;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fourcc;
    std::uint32_t stride;
    std::uint32_t payloadSize;
    std::uint32_t flags;
    std::uint64_t seq;
    std::uint64_t producerQpc100ns;
};

static_assert(sizeof(FrameHeader) == kHeaderSize);
static_assert(offsetof(FrameHeader, width) == 8);
static_assert(offsetof(FrameHeader, fourcc) == 16);
static_assert(offsetof(FrameHeader, flags) == 28);
static_assert(offsetof(FrameHeader, seq) == 32);
static_assert(offsetof(FrameHeader, producerQpc100ns) == 40);

struct VideoMode {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fpsNum;
    std::uint32_t fpsDen;

    friend constexpr bool operator==(const VideoMode&, const VideoMode&) = default;
};

// Bytes of one tightly packed NV12 frame (Y plane + interleaved UV plane at half height).
[[nodiscard]] constexpr std::uint32_t Nv12FrameBytes(std::uint32_t width, std::uint32_t height) noexcept {
    return width * height * 3u / 2u;
}

// ---- Mode catalog (protocol/FRAME_PIPE.md §1) ------------------------------------------------
// The only modes the installer, the hub and every consumer accept. Anything else is rejected, so
// a corrupted registry value or subscription can never make a component allocate an arbitrary
// buffer.

struct FrameSize {
    std::uint32_t width;
    std::uint32_t height;

    friend constexpr bool operator==(const FrameSize&, const FrameSize&) = default;
};

// Largest first: the advertising order.
inline constexpr std::array<FrameSize, 7> kCatalogSizes{{
    {3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {960, 540}, {640, 480}, {640, 360},
}};
// Highest first: the advertising order.
inline constexpr std::array<std::uint32_t, 3> kCatalogFps{60, 30, 15};
// Raw NV12 at 3840x2160@60 is ~750 MB/s per consumer; 4K is offered up to this rate.
inline constexpr std::uint32_t kMaxFpsAbove1440p = 30;
inline constexpr std::size_t kMaxCatalogModes = kCatalogSizes.size() * kCatalogFps.size();

[[nodiscard]] constexpr bool IsCatalogSize(std::uint32_t width, std::uint32_t height) noexcept {
    for (const FrameSize& size : kCatalogSizes) {
        if (size.width == width && size.height == height) return true;
    }
    return false;
}

[[nodiscard]] constexpr std::uint32_t MaxCatalogFps(std::uint32_t width, std::uint32_t height) noexcept {
    return width * height > 2560u * 1440u ? kMaxFpsAbove1440p : kCatalogFps.front();
}

// True for catalog modes (size, integer frame rate and the per-size frame rate limit).
[[nodiscard]] constexpr bool IsSupportedMode(const VideoMode& mode) noexcept {
    if (!IsCatalogSize(mode.width, mode.height) || mode.fpsDen != 1) return false;
    bool fpsOk = false;
    for (const std::uint32_t fps : kCatalogFps) fpsOk = fpsOk || fps == mode.fpsNum;
    return fpsOk && mode.fpsNum <= MaxCatalogFps(mode.width, mode.height);
}

// Frames on the ingest pipe (and placeholders) keep the size the phone encodes, which need not be
// a catalog size (portrait, or a size the phone fell back to): even sides, at most 3840 on either
// side and no more pixels than 3840x2160. Consumers get them scaled to their subscribed mode.
[[nodiscard]] constexpr bool IsIngestSize(std::uint32_t width, std::uint32_t height) noexcept {
    return width >= 2 && height >= 2 && width % 2 == 0 && height % 2 == 0 && width <= kMaxWidth &&
           height <= kMaxWidth && std::uint64_t{width} * height <= std::uint64_t{kMaxWidth} * kMaxHeight;
}

// Installed limit on the advertised modes (registry MaxWidth, MaxHeight, MaxFps).
struct ModeCap {
    std::uint32_t maxWidth;
    std::uint32_t maxHeight;
    std::uint32_t maxFps;

    friend constexpr bool operator==(const ModeCap&, const ModeCap&) = default;
};

inline constexpr ModeCap kFullCatalogCap{kMaxWidth, kMaxHeight, kCatalogFps.front()};

[[nodiscard]] constexpr bool Admits(const ModeCap& cap, const VideoMode& mode) noexcept {
    return mode.width <= cap.maxWidth && mode.height <= cap.maxHeight && mode.fpsNum <= cap.maxFps * mode.fpsDen;
}

// A cap is valid when its frame rate is a catalog rate and it admits at least the smallest size.
[[nodiscard]] constexpr bool IsValidCap(const ModeCap& cap) noexcept {
    bool fpsOk = false;
    for (const std::uint32_t fps : kCatalogFps) fpsOk = fpsOk || fps == cap.maxFps;
    const FrameSize& smallest = kCatalogSizes.back();
    return fpsOk && cap.maxWidth >= smallest.width && cap.maxHeight >= smallest.height &&
           cap.maxWidth <= kMaxWidth && cap.maxHeight <= kMaxHeight;
}

// True when `mode` is a catalog mode the cap lets through: the modes a consumer may subscribe to.
[[nodiscard]] constexpr bool IsAdvertised(const ModeCap& cap, const VideoMode& mode) noexcept {
    return IsSupportedMode(mode) && Admits(cap, mode);
}

// The advertised modes in advertising order: `defaultMode` first (when advertised), then the
// others from the largest to the smallest size and the highest to the lowest frame rate.
class ModeList {
public:
    [[nodiscard]] constexpr const VideoMode* begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const VideoMode* end() const noexcept { return items_.data() + count_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr const VideoMode& operator[](std::size_t index) const noexcept { return items_[index]; }

    [[nodiscard]] constexpr bool Contains(const VideoMode& mode) const noexcept {
        for (const VideoMode& item : *this) {
            if (item == mode) return true;
        }
        return false;
    }

    constexpr void Add(const VideoMode& mode) noexcept {
        if (count_ < items_.size() && !Contains(mode)) items_[count_++] = mode;
    }

private:
    std::array<VideoMode, kMaxCatalogModes> items_{};
    std::size_t count_ = 0;
};

[[nodiscard]] constexpr ModeList AdvertisedModes(const ModeCap& cap, const VideoMode& defaultMode) noexcept {
    ModeList list;
    if (IsAdvertised(cap, defaultMode)) list.Add(defaultMode);
    for (const FrameSize& size : kCatalogSizes) {
        for (const std::uint32_t fps : kCatalogFps) {
            const VideoMode mode{size.width, size.height, fps, 1};
            if (IsAdvertised(cap, mode)) list.Add(mode);
        }
    }
    return list;
}

// The mode of `modes` with this size and frame rate (a ratio). A zero rate means "unspecified" and
// picks the first mode of that size, in advertising order. Used to map the format an application
// selected back to a mode.
[[nodiscard]] constexpr std::optional<VideoMode> FindMode(const ModeList& modes, std::uint32_t width, std::uint32_t height,
                                                          std::uint32_t fpsNum, std::uint32_t fpsDen) noexcept {
    for (const VideoMode& mode : modes) {
        if (mode.width != width || mode.height != height) continue;
        if (fpsNum == 0 || fpsDen == 0) return mode;
        if (std::uint64_t{fpsNum} * mode.fpsDen == std::uint64_t{mode.fpsNum} * fpsDen) return mode;
    }
    return std::nullopt;
}

// Same with a frame duration in 100 ns units (DirectShow AvgTimePerFrame); 0 means unspecified.
// Applications round durations differently (333333 or 333334 for 30 fps), so a duration within
// 0.5 % of a mode's nominal duration matches it.
[[nodiscard]] constexpr std::optional<VideoMode> FindModeByDuration(const ModeList& modes, std::uint32_t width,
                                                                    std::uint32_t height, std::int64_t duration100ns) noexcept {
    for (const VideoMode& mode : modes) {
        if (mode.width != width || mode.height != height) continue;
        if (duration100ns == 0) return mode;
        const std::int64_t nominal = 10'000'000ll * mode.fpsDen / mode.fpsNum;
        const std::int64_t difference = duration100ns > nominal ? duration100ns - nominal : nominal - duration100ns;
        if (difference * 200 <= nominal) return mode;
    }
    return std::nullopt;
}

// ---- Subscription (consumer → hub, protocol/FRAME_PIPE.md §3.1) ------------------------------

inline constexpr std::uint32_t kSubscribeMagic = 0x5342574Du;  // bytes 'M' 'W' 'B' 'S'
inline constexpr std::uint16_t kSubscribeSize = 24;

struct SubscribeRequest {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fpsNum;
    std::uint32_t fpsDen;
};

static_assert(sizeof(SubscribeRequest) == kSubscribeSize);
static_assert(offsetof(SubscribeRequest, width) == 8);
static_assert(offsetof(SubscribeRequest, fpsDen) == 20);

[[nodiscard]] constexpr SubscribeRequest MakeSubscribeRequest(const VideoMode& mode) noexcept {
    return SubscribeRequest{
        .magic = kSubscribeMagic,
        .version = kVersion,
        .size = kSubscribeSize,
        .width = mode.width,
        .height = mode.height,
        .fpsNum = mode.fpsNum,
        .fpsDen = mode.fpsDen,
    };
}

[[nodiscard]] constexpr VideoMode ModeOf(const SubscribeRequest& request) noexcept {
    return VideoMode{request.width, request.height, request.fpsNum, request.fpsDen};
}

enum class SubscribeError {
    None,
    BadMagic,
    BadVersion,
    BadSize,
    NotAdvertised,
};

[[nodiscard]] constexpr SubscribeError ValidateSubscribe(const SubscribeRequest& request, const ModeCap& cap) noexcept {
    if (request.magic != kSubscribeMagic) return SubscribeError::BadMagic;
    if (request.version != kVersion) return SubscribeError::BadVersion;
    if (request.size != kSubscribeSize) return SubscribeError::BadSize;
    if (!IsAdvertised(cap, ModeOf(request))) return SubscribeError::NotAdvertised;
    return SubscribeError::None;
}

[[nodiscard]] constexpr const char* ToString(SubscribeError error) noexcept {
    switch (error) {
        case SubscribeError::None: return "none";
        case SubscribeError::BadMagic: return "bad magic";
        case SubscribeError::BadVersion: return "bad version";
        case SubscribeError::BadSize: return "bad size";
        case SubscribeError::NotAdvertised: return "mode not advertised";
    }
    return "unknown";
}

// ---- Frame message (hub → consumer, protocol/FRAME_PIPE.md §3.2) -----------------------------

enum class ValidationError {
    None,
    BadMagic,
    BadVersion,
    BadHeaderSize,
    BadFourcc,
    ModeMismatch,
    BadStride,
    BadPayloadSize,
    UnknownFlags,
};

[[nodiscard]] constexpr ValidationError Validate(const FrameHeader& header,
                                                 std::uint32_t expectedWidth,
                                                 std::uint32_t expectedHeight) noexcept {
    if (header.magic != kMagic) return ValidationError::BadMagic;
    if (header.version != kVersion) return ValidationError::BadVersion;
    if (header.headerSize != kHeaderSize) return ValidationError::BadHeaderSize;
    if (header.fourcc != kFourccNv12) return ValidationError::BadFourcc;
    if (header.width != expectedWidth || header.height != expectedHeight) return ValidationError::ModeMismatch;
    if (header.stride != header.width) return ValidationError::BadStride;
    if (header.payloadSize != Nv12FrameBytes(header.stride, header.height)) return ValidationError::BadPayloadSize;
    if ((header.flags & ~kKnownFlags) != 0) return ValidationError::UnknownFlags;
    return ValidationError::None;
}

[[nodiscard]] constexpr FrameHeader MakeHeader(std::uint32_t width, std::uint32_t height, std::uint32_t flags,
                                               std::uint64_t seq, std::uint64_t producerQpc100ns) noexcept {
    return FrameHeader{
        .magic = kMagic,
        .version = kVersion,
        .headerSize = kHeaderSize,
        .width = width,
        .height = height,
        .fourcc = kFourccNv12,
        .stride = width,
        .payloadSize = Nv12FrameBytes(width, height),
        .flags = flags,
        .seq = seq,
        .producerQpc100ns = producerQpc100ns,
    };
}

[[nodiscard]] constexpr const char* ToString(ValidationError error) noexcept {
    switch (error) {
        case ValidationError::None: return "none";
        case ValidationError::BadMagic: return "bad magic";
        case ValidationError::BadVersion: return "bad version";
        case ValidationError::BadHeaderSize: return "bad header size";
        case ValidationError::BadFourcc: return "bad fourcc";
        case ValidationError::ModeMismatch: return "mode mismatch";
        case ValidationError::BadStride: return "bad stride";
        case ValidationError::BadPayloadSize: return "bad payload size";
        case ValidationError::UnknownFlags: return "unknown flags";
    }
    return "unknown";
}

}  // namespace mwb::frame
