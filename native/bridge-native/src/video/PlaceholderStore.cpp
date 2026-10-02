#include "video/PlaceholderStore.h"

#include <windows.h>

#include <algorithm>

#include <mwb/FrameProtocol.h>
#include <wil/resource.h>

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

namespace {

constexpr std::uint8_t kNeutralLuma = 40;     // dark grey (limited range black is 16)
constexpr std::uint8_t kNeutralChroma = 128;  // no colour

std::size_t Index(PlaceholderKind kind) noexcept { return static_cast<std::size_t>(kind); }

}  // namespace

FrameBytes ReadPlaceholderFile(const std::filesystem::path& path, std::size_t frameBytes) {
    const std::string name = ToUtf8(path.native());
    const HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) throw ErrorFromWin32(::GetLastError(), "Cannot open placeholder " + name);
    const wil::unique_handle file(raw);

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) throw ErrorFromWin32(::GetLastError(), "Cannot read placeholder " + name);
    if (static_cast<unsigned long long>(size.QuadPart) != frameBytes) {
        throw CommandError(ErrorCode::IoError, "Placeholder " + name + " has " + std::to_string(size.QuadPart) +
                                                   " bytes; expected " + std::to_string(frameBytes));
    }

    FrameBytes frame(frameBytes);
    std::size_t offset = 0;
    while (offset < frameBytes) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(frameBytes - offset, 1u << 20));
        if (!::ReadFile(file.get(), frame.data() + offset, chunk, &read, nullptr) || read == 0) {
            throw ErrorFromWin32(::GetLastError(), "Cannot read placeholder " + name);
        }
        offset += read;
    }
    return frame;
}

PlaceholderStore::PlaceholderStore(std::uint32_t width, std::uint32_t height) {
    const std::size_t lumaBytes = static_cast<std::size_t>(width) * height;
    FrameBytes neutral(mwb::frame::Nv12FrameBytes(width, height), kNeutralChroma);
    std::fill_n(neutral.begin(), lumaBytes, kNeutralLuma);
    neutral_ = PlaceholderFrame{std::make_shared<const FrameBytes>(std::move(neutral)), width, height};
}

void PlaceholderStore::Set(PlaceholderKind kind, FrameBytes frame, std::uint32_t width, std::uint32_t height) {
    frames_[Index(kind)] = PlaceholderFrame{std::make_shared<const FrameBytes>(std::move(frame)), width, height};
}

const PlaceholderFrame& PlaceholderStore::Get(PlaceholderKind kind) const {
    const PlaceholderFrame& frame = frames_[Index(kind)];
    return frame.bytes ? frame : neutral_;
}

bool PlaceholderStore::IsLoaded(PlaceholderKind kind) const noexcept { return frames_[Index(kind)].bytes != nullptr; }

}  // namespace mwb::native
