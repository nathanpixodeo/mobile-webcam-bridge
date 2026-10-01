// MobileWebcamBridge virtual microphone — PCM feed buffer.
//
// Owns the ring buffer between the feed control device (writer, IOCTL path at PASSIVE_LEVEL)
// and the capture stream (reader, position updates at DISPATCH_LEVEL). One spinlock serialises
// both sides. The reader is always called with the stream's position spinlock held, so the
// global lock order is: stream position lock -> feed lock. The writer never takes the
// position lock.
//
// Policy (protocol/MIC_FEED.md): audio written while no capture stream runs is discarded, the
// oldest audio is dropped on overrun, and the capture stream reads silence on underrun.
#pragma once

#include <mwb/MwbMicIoctl.h>
#include <mwb/PcmRing.h>

class MicFeedBuffer final
{
public:
    MicFeedBuffer();
    ~MicFeedBuffer();

    MicFeedBuffer(const MicFeedBuffer&) = delete;
    MicFeedBuffer& operator=(const MicFeedBuffer&) = delete;

    // Allocates the non-paged ring storage. Until it succeeds the capture stream reads silence
    // and writes are discarded.
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS Initialize();

    // Appends PCM (length already validated: even, <= MWBMIC_MAX_WRITE_BYTES) and returns the
    // status after the write. `data` is fully consumed before `status` is produced, so both
    // may point into the same IRP system buffer.
    _IRQL_requires_max_(DISPATCH_LEVEL)
    void Write(_In_reads_bytes_(length) const UCHAR* data, _In_ ULONG length, _Out_ MWBMIC_STATUS* status);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void QueryStatus(_Out_ MWBMIC_STATUS* status);

    // Drops all buffered audio (feeder handle closed, IOCTL_MWBMIC_RESET).
    _IRQL_requires_max_(DISPATCH_LEVEL)
    void Reset();

    // Called by the capture stream on KSSTATE_RUN (true) and PAUSE/STOP (false). Every
    // transition flushes the ring so a new stream never starts with stale audio.
    _IRQL_requires_max_(DISPATCH_LEVEL)
    void SetStreamActive(_In_ bool active);

    // Fills `length` bytes of the capture DMA buffer: buffered audio first, then silence.
    // The caller holds the stream position spinlock (hence already at DISPATCH_LEVEL).
    _IRQL_requires_(DISPATCH_LEVEL)
    void ReadForCapture(_Out_writes_bytes_all_(length) UCHAR* destination, _In_ ULONG length);

private:
    void FillStatusLocked(_Out_ MWBMIC_STATUS* status) const;

    KSPIN_LOCK      m_lock;
    UCHAR*          m_storage;
    mwb::PcmRing    m_ring;
    bool            m_streamActive;
    ULONG           m_underrunCount;
    ULONG           m_overrunCount;
    ULONGLONG       m_totalBytesWritten;
    ULONGLONG       m_totalBytesRead;
};
