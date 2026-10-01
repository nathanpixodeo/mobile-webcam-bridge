// Byte ring buffer for PCM audio, shared by mwbmic.sys and the user-mode unit tests.
//
// Kernel-safe on purpose: no exceptions, no allocation, no standard library. The ring does not
// own its storage and does no locking — the driver serialises every call with its spinlock.
//
// Policy (protocol/MIC_FEED.md): writers never block, the oldest audio is dropped on overrun;
// readers always get the requested amount, zero-filled (silence) on underrun.
#pragma once

#ifdef _KERNEL_MODE
// The including translation unit has already included <wdm.h> (directly or via <portcls.h>).
#else
#include <string.h>
#endif

namespace mwb {

class PcmRing final {
public:
    using Byte = unsigned char;
    using Size = unsigned int;
    static_assert(sizeof(Size) == 4, "Size must be 32-bit");

    PcmRing() = default;
    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    // `capacity` must be a non-zero multiple of `blockAlign` (bytes per audio frame).
    // Returns false (and leaves the ring detached) when the arguments are invalid.
    bool Attach(Byte* storage, Size capacity, Size blockAlign) noexcept {
        if (storage == nullptr || blockAlign == 0 || capacity == 0 || capacity % blockAlign != 0) {
            Detach();
            return false;
        }
        storage_ = storage;
        capacity_ = capacity;
        blockAlign_ = blockAlign;
        Clear();
        return true;
    }

    void Detach() noexcept {
        storage_ = nullptr;
        capacity_ = 0;
        blockAlign_ = 1;
        Clear();
    }

    void Clear() noexcept {
        readIndex_ = 0;
        size_ = 0;
    }

    [[nodiscard]] bool IsAttached() const noexcept { return storage_ != nullptr; }
    [[nodiscard]] Size Capacity() const noexcept { return capacity_; }
    [[nodiscard]] Size BufferedBytes() const noexcept { return size_; }
    [[nodiscard]] Size FreeBytes() const noexcept { return capacity_ - size_; }

    // Appends `length` bytes. `length` is rounded down to whole audio frames. When the data does
    // not fit, the oldest buffered frames are discarded first; if `length` alone exceeds the
    // capacity only its newest `capacity` bytes are kept. Returns the number of bytes dropped
    // (old buffered bytes plus skipped input bytes), so the caller can count overruns.
    Size WriteDropOldest(const Byte* data, Size length) noexcept {
        if (!IsAttached() || data == nullptr) return 0;
        length -= length % blockAlign_;
        if (length == 0) return 0;

        Size dropped = 0;
        if (length > capacity_) {
            const Size skip = length - capacity_;
            data += skip;
            length = capacity_;
            dropped += skip;
        }
        if (length > FreeBytes()) {
            const Size discard = length - FreeBytes();
            readIndex_ = Wrap(readIndex_ + discard);
            size_ -= discard;
            dropped += discard;
        }

        Size writeIndex = Wrap(readIndex_ + size_);
        Size remaining = length;
        while (remaining > 0) {
            const Size chunk = Min(remaining, capacity_ - writeIndex);
            Copy(storage_ + writeIndex, data, chunk);
            data += chunk;
            remaining -= chunk;
            writeIndex = Wrap(writeIndex + chunk);
        }
        size_ += length;
        return dropped;
    }

    // Fills exactly `length` bytes of `destination`: buffered audio first, then silence.
    // Returns the number of bytes taken from the ring (less than `length` means underrun).
    Size ReadZeroFill(Byte* destination, Size length) noexcept {
        if (destination == nullptr || length == 0) return 0;
        const Size available = IsAttached() ? Min(length, size_) : 0;

        Size copied = 0;
        while (copied < available) {
            const Size chunk = Min(available - copied, capacity_ - readIndex_);
            Copy(destination + copied, storage_ + readIndex_, chunk);
            copied += chunk;
            readIndex_ = Wrap(readIndex_ + chunk);
        }
        size_ -= available;
        if (available < length) Zero(destination + available, length - available);
        return available;
    }

private:
    [[nodiscard]] static Size Min(Size a, Size b) noexcept { return a < b ? a : b; }
    [[nodiscard]] Size Wrap(Size index) const noexcept { return index >= capacity_ ? index - capacity_ : index; }

    static void Copy(Byte* destination, const Byte* source, Size length) noexcept {
#ifdef _KERNEL_MODE
        RtlCopyMemory(destination, source, length);
#else
        memcpy(destination, source, length);
#endif
    }

    static void Zero(Byte* destination, Size length) noexcept {
#ifdef _KERNEL_MODE
        RtlZeroMemory(destination, length);
#else
        memset(destination, 0, length);
#endif
    }

    Byte* storage_ = nullptr;
    Size capacity_ = 0;
    Size blockAlign_ = 1;
    Size readIndex_ = 0;
    Size size_ = 0;
};

}  // namespace mwb
