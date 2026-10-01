// MobileWebcamBridge virtual microphone — PCM feed buffer.

#include "definitions.h"
#include "MicFeedBuffer.h"

// Every method may run at DISPATCH_LEVEL or with a spinlock held: keep it all resident.
#pragma code_seg()

MicFeedBuffer::MicFeedBuffer()
    : m_storage(nullptr),
      m_streamActive(false),
      m_underrunCount(0),
      m_overrunCount(0),
      m_totalBytesWritten(0),
      m_totalBytesRead(0)
{
    KeInitializeSpinLock(&m_lock);
}

MicFeedBuffer::~MicFeedBuffer()
{
    m_ring.Detach();

    if (m_storage != nullptr)
    {
        ExFreePoolWithTag(m_storage, MWBMIC_FEED_POOLTAG);
        m_storage = nullptr;
    }
}

_Use_decl_annotations_
NTSTATUS MicFeedBuffer::Initialize()
{
    if (m_storage != nullptr)
    {
        return STATUS_SUCCESS;
    }

    // POOL_FLAG_NON_PAGED allocations are non-executable (HVCI compatible).
    UCHAR* storage = static_cast<UCHAR*>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, MWBMIC_RING_CAPACITY_BYTES, MWBMIC_FEED_POOLTAG));
    if (storage == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    const bool attached = m_ring.Attach(storage, MWBMIC_RING_CAPACITY_BYTES, MWBMIC_BLOCK_ALIGN);
    if (attached)
    {
        m_storage = storage;
    }
    KeReleaseSpinLock(&m_lock, oldIrql);

    if (!attached)
    {
        ExFreePoolWithTag(storage, MWBMIC_FEED_POOLTAG);
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void MicFeedBuffer::Write(const UCHAR* data, ULONG length, MWBMIC_STATUS* status)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (m_streamActive && length > 0 && m_ring.IsAttached())
    {
        const ULONG whole = length - (length % MWBMIC_BLOCK_ALIGN);
        const ULONG accepted = whole < m_ring.Capacity() ? whole : m_ring.Capacity();

        if (m_ring.WriteDropOldest(data, length) > 0)
        {
            ++m_overrunCount;
        }
        m_totalBytesWritten += accepted;
    }

    FillStatusLocked(status);

    KeReleaseSpinLock(&m_lock, oldIrql);
}

_Use_decl_annotations_
void MicFeedBuffer::QueryStatus(MWBMIC_STATUS* status)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    FillStatusLocked(status);
    KeReleaseSpinLock(&m_lock, oldIrql);
}

_Use_decl_annotations_
void MicFeedBuffer::Reset()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    m_ring.Clear();
    KeReleaseSpinLock(&m_lock, oldIrql);
}

_Use_decl_annotations_
void MicFeedBuffer::SetStreamActive(bool active)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    m_streamActive = active;
    m_ring.Clear();
    KeReleaseSpinLock(&m_lock, oldIrql);
}

_Use_decl_annotations_
void MicFeedBuffer::ReadForCapture(UCHAR* destination, ULONG length)
{
    KeAcquireSpinLockAtDpcLevel(&m_lock);

    // ReadZeroFill always fills `length` bytes, also when the ring is not attached.
    const ULONG read = m_ring.ReadZeroFill(destination, length);
    m_totalBytesRead += read;
    if (read < length)
    {
        ++m_underrunCount;
    }

    KeReleaseSpinLockFromDpcLevel(&m_lock);
}

_Use_decl_annotations_
void MicFeedBuffer::FillStatusLocked(MWBMIC_STATUS* status) const
{
    RtlZeroMemory(status, sizeof(*status));
    status->StructSize        = sizeof(MWBMIC_STATUS);
    status->Flags             = m_streamActive ? MWBMIC_STATUS_FLAG_STREAM_ACTIVE : 0u;
    status->BufferedBytes     = m_ring.BufferedBytes();
    status->CapacityBytes     = m_ring.Capacity();
    status->UnderrunCount     = m_underrunCount;
    status->OverrunCount      = m_overrunCount;
    status->TotalBytesWritten = m_totalBytesWritten;
    status->TotalBytesRead    = m_totalBytesRead;
}
