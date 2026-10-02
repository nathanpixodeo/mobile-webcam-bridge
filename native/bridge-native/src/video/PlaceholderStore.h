// NV12 placeholder frames: the ones Node renders and loads per kind (at any size; consumers get
// them scaled), plus a built-in neutral frame used whenever the requested kind was never loaded.
// Not thread-safe.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "video/FramePool.h"
#include "video/OutputPolicy.h"

namespace mwb::native {

// Reads a raw NV12 file; it must be exactly `frameBytes` long. Throws CommandError(IoError).
[[nodiscard]] FrameBytes ReadPlaceholderFile(const std::filesystem::path& path, std::size_t frameBytes);

struct PlaceholderFrame {
    SharedFrameBytes bytes;  // tightly packed NV12
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class PlaceholderStore {
public:
    // Size of the neutral frame.
    PlaceholderStore(std::uint32_t width, std::uint32_t height);

    void Set(PlaceholderKind kind, FrameBytes frame, std::uint32_t width, std::uint32_t height);

    // The frame for `kind`, or the neutral frame when none was loaded.
    [[nodiscard]] const PlaceholderFrame& Get(PlaceholderKind kind) const;
    [[nodiscard]] bool IsLoaded(PlaceholderKind kind) const noexcept;

private:
    std::array<PlaceholderFrame, kPlaceholderKindCount> frames_{};
    PlaceholderFrame neutral_;
};

}  // namespace mwb::native
