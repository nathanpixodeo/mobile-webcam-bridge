#include "mic/MicFeeder.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include <wil/resource.h>

#include "core/Json.h"
#include "core/Output.h"
#include "mic/PcmChunker.h"
#include "platform/Stdin.h"

namespace mwb::native {

namespace {

constexpr std::size_t kChunkBytes = MWBMIC_SAMPLE_RATE / 100 * MWBMIC_BLOCK_ALIGN;  // 10 ms
constexpr DWORD kStatusIntervalMs = 100;

}  // namespace

CommandError MicOpenFailure(MicOpenError error, unsigned long win32Error) {
    switch (error) {
        case MicOpenError::NotPresent:
            return CommandError(ErrorCode::DeviceNotPresent,
                                "The virtual microphone is not installed or not started (\\\\.\\MobileWebcamBridgeMic missing)");
        case MicOpenError::Busy:
            return CommandError(ErrorCode::DeviceBusy, "Another process is already feeding the virtual microphone");
        case MicOpenError::AccessDenied:
            return CommandError(ErrorCode::AccessDenied, "Access to the virtual microphone was denied");
        case MicOpenError::None:
        case MicOpenError::Other: break;
    }
    return ErrorFromWin32(win32Error, "Cannot open the virtual microphone");
}

ExitCode MicFeeder::Run() {
    MicOpenError openError = MicOpenError::None;
    DWORD win32Error = ERROR_SUCCESS;
    std::optional<MicDevice> device = MicDevice::Open(openError, win32Error);
    if (!device) throw MicOpenFailure(openError, win32Error);

    EmitReady(device->QueryVersion());

    // Shared between the feeding (main) thread and the status thread.
    std::mutex mutex;
    std::optional<MicStatus> latestWrite;
    std::optional<CommandError> statusFailure;
    wil::unique_event stop;
    stop.create(wil::EventOptions::ManualReset);

    HANDLE mainThreadRaw = nullptr;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &mainThreadRaw, 0, FALSE,
                      DUPLICATE_SAME_ACCESS);
    const wil::unique_handle mainThread(mainThreadRaw);

    std::thread statusThread([&] {
        while (::WaitForSingleObject(stop.get(), kStatusIntervalMs) == WAIT_TIMEOUT) {
            try {
                std::optional<MicStatus> status;
                {
                    const std::lock_guard lock(mutex);
                    status.swap(latestWrite);  // fresh status from the last write, if any
                }
                if (!status) status = device->QueryStatus();
                EmitStatus(*status);
            } catch (...) {
                {
                    const std::lock_guard lock(mutex);
                    statusFailure = CurrentExceptionToCommandError();
                }
                if (mainThread) ::CancelSynchronousIo(mainThread.get());  // unblock the stdin read
                return;
            }
        }
    });
    const auto stopStatusThread = wil::scope_exit([&] {
        stop.SetEvent();
        statusThread.join();
    });

    StdinReader reader;
    PcmChunker chunker(kChunkBytes, MWBMIC_BLOCK_ALIGN);
    const auto write = [&](std::span<const std::uint8_t> chunk) {
        const MicStatus status = device->Write(chunk);
        const std::lock_guard lock(mutex);
        latestWrite = status;
    };

    std::uint8_t buffer[16 * 1024];
    while (true) {
        const std::size_t read = reader.Read(buffer, sizeof(buffer));
        if (read == 0) break;
        chunker.Push(std::span<const std::uint8_t>(buffer, read), write);
    }
    {
        const std::lock_guard lock(mutex);
        if (statusFailure) throw *statusFailure;
    }
    chunker.Flush(write);
    return ExitCode::Success;
}

void MicFeeder::EmitReady(const MWBMIC_VERSION& version) {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "ready")
        .Field("abi", version.AbiVersion)
        .Field("sampleRate", version.SampleRate)
        .Field("channels", static_cast<std::uint32_t>(version.Channels))
        .Field("capacityBytes", version.RingCapacityBytes)
        .EndObject();
    console_.Emit(writer);
}

void MicFeeder::EmitStatus(const MicStatus& status) {
    JsonWriter writer;
    writer.BeginObject()
        .Field("event", "status")
        .Field("bufferedBytes", status.bufferedBytes)
        .Field("capacityBytes", status.capacityBytes)
        .Field("streamActive", status.streamActive)
        .Field("underruns", status.underruns)
        .Field("overruns", status.overruns)
        .EndObject();
    console_.Emit(writer);
}

}  // namespace mwb::native
