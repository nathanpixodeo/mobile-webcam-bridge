// Frame pipe protocol v1 — the contract between `bridge-native video hub` and the camera
// components (vcam-mf.dll, vcam-dshow.dll). Specification: protocol/FRAME_PIPE.md.
//
// User-mode only. Header-only, no Windows dependencies, so it is unit-testable anywhere.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace mwb::frame {

static_assert(std::endian::native == std::endian::little, "Frame pipe protocol is little-endian");

inline constexpr std::uint32_t kMagic = 0x5642574Du;       // bytes 'M' 'W' 'B' 'V'
inline constexpr std::uint16_t kVersion = 1;
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

// Camera modes accepted by the installer and every consumer. Anything else is rejected, so a
// corrupted registry value can never make a privileged consumer allocate an arbitrary buffer.
[[nodiscard]] constexpr bool IsSupportedMode(const VideoMode& mode) noexcept {
    const bool sizeOk = (mode.width == 640 && mode.height == 360) ||
                        (mode.width == 1280 && mode.height == 720) ||
                        (mode.width == 1920 && mode.height == 1080);
    const bool fpsOk = mode.fpsDen == 1 && (mode.fpsNum == 15 || mode.fpsNum == 24 ||
                                            mode.fpsNum == 25 || mode.fpsNum == 30 ||
                                            mode.fpsNum == 60);
    return sizeOk && fpsOk;
}

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
