// NV12 placeholder frames: the ones Node renders and loads per kind, plus a built-in neutral
// frame used whenever the requested kind was never loaded. Not thread-safe.
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

#include "video/FramePool.h"
#include "video/OutputPolicy.h"

namespace mwb::native {

// Reads a raw NV12 file; it must be exactly `frameBytes` long. Throws CommandError(IoError).
[[nodiscard]] FrameBytes ReadPlaceholderFile(const std::filesystem::path& path, std::size_t frameBytes);

class PlaceholderStore {
public:
    PlaceholderStore(std::uint32_t width, std::uint32_t height);

    void Set(PlaceholderKind kind, FrameBytes frame);

    // The frame for `kind`, or the neutral frame when none was loaded.
    [[nodiscard]] SharedFrameBytes Get(PlaceholderKind kind) const;
    [[nodiscard]] bool IsLoaded(PlaceholderKind kind) const noexcept;

private:
    std::array<SharedFrameBytes, kPlaceholderKindCount> frames_{};
    SharedFrameBytes neutral_;
};

}  // namespace mwb::native
