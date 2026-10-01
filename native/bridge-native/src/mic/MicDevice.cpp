#include "mic/MicDevice.h"

#include <string>

#include "core/Errors.h"

namespace mwb::native {

namespace {

[[noreturn]] void ThrowIoctlFailure(DWORD error, const char* context) {
    const bool removed = error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_FILE_NOT_FOUND ||
                         error == ERROR_DEVICE_REMOVED || error == ERROR_NO_SUCH_DEVICE;
    if (removed) throw CommandError(ErrorCode::DeviceNotPresent, std::string(context) + ": the virtual microphone was removed");
    throw ErrorFromWin32(error, context);
}

MicStatus ToMicStatus(const MWBMIC_STATUS& status, DWORD bytes) {
    if (bytes < sizeof(MWBMIC_STATUS) || status.StructSize < sizeof(MWBMIC_STATUS)) {
        throw CommandError(ErrorCode::Internal, "The virtual microphone driver returned a short status");
    }
    MicStatus result;
    result.bufferedBytes = status.BufferedBytes;
    result.capacityBytes = status.CapacityBytes;
    result.streamActive = (status.Flags & MWBMIC_STATUS_FLAG_STREAM_ACTIVE) != 0;
    result.underruns = status.UnderrunCount;
    result.overruns = status.OverrunCount;
    return result;
}

}  // namespace

std::optional<MicDevice> MicDevice::Open(MicOpenError& error, DWORD& win32Error) {
    const HANDLE raw = ::CreateFileW(MWBMIC_USER_PATH_W, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        win32Error = ::GetLastError();
        switch (win32Error) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_DEVICE_NOT_CONNECTED: error = MicOpenError::NotPresent; break;
            case ERROR_SHARING_VIOLATION:
            case ERROR_BUSY: error = MicOpenError::Busy; break;
            case ERROR_ACCESS_DENIED: error = MicOpenError::AccessDenied; break;
            default: error = MicOpenError::Other; break;
        }
        return std::nullopt;
    }
    error = MicOpenError::None;
    win32Error = ERROR_SUCCESS;
    return MicDevice(wil::unique_handle(raw));
}

MWBMIC_VERSION MicDevice::QueryVersion() const {
    MWBMIC_VERSION version{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(handle_.get(), IOCTL_MWBMIC_GET_VERSION, nullptr, 0, &version, sizeof(version), &bytes, nullptr)) {
        ThrowIoctlFailure(::GetLastError(), "Querying the virtual microphone driver version failed");
    }
    const bool compatible = bytes >= sizeof(version) && version.StructSize >= sizeof(version) &&
                            version.AbiVersion == MWBMIC_ABI_VERSION && version.SampleRate == MWBMIC_SAMPLE_RATE &&
                            version.Channels == MWBMIC_CHANNELS && version.BitsPerSample == MWBMIC_BITS_PER_SAMPLE;
    if (!compatible) {
        throw CommandError(ErrorCode::Internal, "The virtual microphone driver speaks ABI " + std::to_string(version.AbiVersion) +
                                                    "; this bridge-native expects ABI " + std::to_string(MWBMIC_ABI_VERSION));
    }
    return version;
}

MicStatus MicDevice::Write(std::span<const std::uint8_t> pcm) const {
    MWBMIC_STATUS status{};
    DWORD bytes = 0;
    // The input buffer is only read; DeviceIoControl's signature is just not const-correct.
    if (!::DeviceIoControl(handle_.get(), IOCTL_MWBMIC_WRITE, const_cast<std::uint8_t*>(pcm.data()),
                           static_cast<DWORD>(pcm.size()), &status, sizeof(status), &bytes, nullptr)) {
        ThrowIoctlFailure(::GetLastError(), "Writing to the virtual microphone failed");
    }
    return ToMicStatus(status, bytes);
}

MicStatus MicDevice::QueryStatus() const {
    MWBMIC_STATUS status{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(handle_.get(), IOCTL_MWBMIC_GET_STATUS, nullptr, 0, &status, sizeof(status), &bytes, nullptr)) {
        ThrowIoctlFailure(::GetLastError(), "Querying the virtual microphone status failed");
    }
    return ToMicStatus(status, bytes);
}

}  // namespace mwb::native
