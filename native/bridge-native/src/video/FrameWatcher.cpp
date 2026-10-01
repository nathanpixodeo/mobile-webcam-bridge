#include "video/FrameWatcher.h"

#include <windows.h>

#include <vector>

#include <mwb/FrameProtocol.h>

#include "core/Errors.h"
#include "core/Strings.h"
#include "platform/NamedPipe.h"

namespace mwb::native {

namespace {

[[noreturn]] void ThrowRead(const IoResult& result, std::uint32_t received, std::uint32_t wanted) {
    if (result.status == IoStatus::TimedOut) {
        throw CommandError(ErrorCode::IoError, "Timed out after " + std::to_string(received) + " of " +
                                                   std::to_string(wanted) + " frames");
    }
    throw ErrorFromWin32(result.error, "Reading frames failed after " + std::to_string(received) + " frames");
}

bool IsPlausibleSize(std::uint32_t width, std::uint32_t height) noexcept {
    return width > 0 && height > 0 && width <= mwb::frame::kMaxWidth && height <= mwb::frame::kMaxHeight &&
           width % 2 == 0 && height % 2 == 0;
}

}  // namespace

WatchReport FrameWatcher::Run() const {
    const std::wstring path = PipePath(settings_.publicPipeName);
    const ULONGLONG start = ::GetTickCount64();
    const ULONGLONG deadline = start + settings_.timeoutMs;
    const auto remaining = [&]() -> DWORD {
        const ULONGLONG now = ::GetTickCount64();
        return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    };

    DWORD error = ERROR_SUCCESS;
    const wil::unique_handle pipe = OpenPipeClient(path, GENERIC_READ, settings_.timeoutMs, error);
    if (!pipe) {
        if (error == ERROR_FILE_NOT_FOUND) {
            throw CommandError(ErrorCode::IoError, "No video hub is running (" + ToUtf8(path) + " not found)");
        }
        throw ErrorFromWin32(error, "Cannot open " + ToUtf8(path));
    }

    WatchReport report;
    if (settings_.expectedMode) {
        report.width = settings_.expectedMode->width;
        report.height = settings_.expectedMode->height;
    }

    OverlappedOperation operation;
    std::vector<std::uint8_t> payload;
    while (report.frames < settings_.frames) {
        mwb::frame::FrameHeader header{};
        IoResult result = ReadExactly(pipe.get(), operation, &header, sizeof(header), nullptr, remaining());
        if (result.status != IoStatus::Completed) ThrowRead(result, report.frames, settings_.frames);

        if (report.width == 0) {  // no installed mode: adopt the first frame's size
            if (!IsPlausibleSize(header.width, header.height)) {
                throw CommandError(ErrorCode::IoError, "First frame has an implausible size");
            }
            report.width = header.width;
            report.height = header.height;
        }
        const mwb::frame::ValidationError validation = mwb::frame::Validate(header, report.width, report.height);
        if (validation != mwb::frame::ValidationError::None) {
            throw CommandError(ErrorCode::IoError, std::string("Invalid frame header: ") + mwb::frame::ToString(validation));
        }

        payload.resize(header.payloadSize);
        result = ReadExactly(pipe.get(), operation, payload.data(), header.payloadSize, nullptr, remaining());
        if (result.status != IoStatus::Completed) ThrowRead(result, report.frames, settings_.frames);

        ++report.frames;
        if ((header.flags & mwb::frame::kFlagPlaceholder) != 0) ++report.placeholderFrames;
    }
    report.elapsedMs = ::GetTickCount64() - start;
    return report;
}

}  // namespace mwb::native
