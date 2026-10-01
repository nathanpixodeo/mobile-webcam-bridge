/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavertstream.cpp

Abstract:

    Implementation of the WaveRT capture stream of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: capture only; WriteBytes() copies PCM from the
    MicFeedBuffer (silence on underrun) instead of generating a sine tone; the
    feed is marked active on KSSTATE_RUN and inactive on PAUSE/STOP; the byte
    displacement is computed in 64-bit arithmetic; only the format's block
    align is kept instead of a copy of the client's WAVEFORMATEX.
--*/

#include "definitions.h"
#include <limits.h>
#include <ks.h>
#include "endpoints.h"
#include "minwavert.h"
#include "minwavertstream.h"
#include "MicFeedBuffer.h"

//=============================================================================
// CMiniportWaveRTStream
//=============================================================================

#pragma code_seg("PAGE")
CMiniportWaveRTStream::~CMiniportWaveRTStream
(
    void
)
{
    PAGED_CODE();

    // Stop the simulated DMA engine first: its DPC uses everything released below.
    if (m_pNotificationTimer)
    {
        ExDeleteTimer
        (
            m_pNotificationTimer,
            TRUE,   // Cancel the timer if it is currently set.
            TRUE,   // Wait for a running callback to finish.
            NULL
        );
        m_pNotificationTimer = NULL;
    }
    KeFlushQueuedDpcs();

    // PortCls stops a stream before closing it; this only covers abnormal teardown.
    if (m_pFeed != NULL && m_KsState == KSSTATE_RUN)
    {
        m_pFeed->SetStreamActive(false);
    }
    m_pFeed = NULL;

    if (NULL != m_pMiniport)
    {
        if (m_bUnregisterStream)
        {
            m_pMiniport->StreamClosed(m_ulPin);
            m_bUnregisterStream = FALSE;
        }

        m_pMiniport->Release();
        m_pMiniport = NULL;
    }

    DPF_ENTER(("[CMiniportWaveRTStream::~CMiniportWaveRTStream]"));
} // ~CMiniportWaveRTStream

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRTStream::Init
(
    _In_ CMiniportWaveRT*           Miniport_,
    _In_ PPORTWAVERTSTREAM          PortStream_,
    _In_ ULONG                      Pin_,
    _In_ PKSDATAFORMAT              DataFormat_,
    _In_ GUID                       SignalProcessingMode
)
{
    PAGED_CODE();

    m_pMiniport = NULL;
    m_pFeed = NULL;
    m_ulPin = 0;
    m_bUnregisterStream = FALSE;
    m_ulDmaBufferSize = 0;
    m_pDmaBuffer = NULL;
    m_ulNotificationsPerBuffer = 0;
    m_KsState = KSSTATE_STOP;
    m_llPacketCounter = 0;
    m_ullPlayPosition = 0;
    m_ullWritePosition = 0;
    m_ullLinearPosition = 0;
    m_ullDmaTimeStamp = 0;
    m_hnsElapsedTimeCarryForward = 0;
    m_ullLastDPCTimeStamp = 0;
    m_hnsDPCTimeCarryForward = 0;
    m_ulDmaMovementRate = 0;
    m_byteDisplacementCarryForward = 0;
    m_ulBlockAlign = 0;
    m_ulLastOsReadPacket = ULONG_MAX;
    m_SignalProcessingMode = SignalProcessingMode;

    m_pPortStream = PortStream_;
    InitializeListHead(&m_NotificationList);
    m_ulNotificationIntervalMs = 0;

    // Synchronises position updates between the timer DPC and the position queries.
    KeInitializeSpinLock(&m_PositionSpinLock);

    m_pNotificationTimer = ExAllocateTimer(TimerNotifyRT, this, EX_TIMER_HIGH_RESOLUTION);
    if (!m_pNotificationTimer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PWAVEFORMATEX pWfEx = GetWaveFormatEx(DataFormat_);
    if (NULL == pWfEx)
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (Miniport_ == NULL || Miniport_->GetAdapterCommon() == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    m_pMiniport = Miniport_;
    m_pMiniport->AddRef();

    m_pFeed = m_pMiniport->GetAdapterCommon()->GetMicFeedBuffer();
    if (m_pFeed == NULL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    m_ulPin = Pin_;
    m_ulDmaMovementRate = pWfEx->nAvgBytesPerSec;
    if (m_ulDmaMovementRate == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Only the frame size is needed later. The sample copied the whole client WAVEFORMATEX,
    // whose cbSize the client controls; this driver keeps just the field it uses.
    m_ulBlockAlign = pWfEx->nBlockAlign;
    if (m_ulBlockAlign == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS ntStatus = m_pMiniport->StreamCreated(m_ulPin);
    if (NT_SUCCESS(ntStatus))
    {
        m_bUnregisterStream = TRUE;
    }

    return ntStatus;
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTInputStream))
    {
        *Object = PVOID(PMINIPORTWAVERTINPUTSTREAM(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::AllocateBufferWithNotification
(
    _In_    ULONG               NotificationCount_,
    _In_    ULONG               RequestedSize_,
    _Out_   PMDL                *AudioBufferMdl_,
    _Out_   ULONG               *ActualSize_,
    _Out_   ULONG               *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE *CacheType_
)
{
    PAGED_CODE();

    if ((0 == RequestedSize_) || (RequestedSize_ < m_ulBlockAlign))
    {
        return STATUS_UNSUCCESSFUL;
    }

    if ((NotificationCount_ == 0) || (RequestedSize_ % NotificationCount_ != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RequestedSize_ -= RequestedSize_ % (m_ulBlockAlign);

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;

    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl(highAddress, RequestedSize_);
    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // The simulated DMA engine writes the buffer from software, so it needs a kernel mapping.
    m_pDmaBuffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    if (NULL == m_pDmaBuffer)
    {
        m_pPortStream->FreePagesFromMdl(pBufferMdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_ulNotificationsPerBuffer = NotificationCount_;
    m_ulDmaBufferSize = RequestedSize_;

    const ULONG ulBufferDurationMs = (RequestedSize_ * 1000) / m_ulDmaMovementRate;
    m_ulNotificationIntervalMs = ulBufferDurationMs / NotificationCount_;

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = RequestedSize_;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::FreeBufferWithNotification
(
    _In_        PMDL    Mdl_,
    _In_        ULONG   Size_
)
{
    UNREFERENCED_PARAMETER(Size_);

    PAGED_CODE();

    if (Mdl_ != NULL)
    {
        if (m_pDmaBuffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(m_pDmaBuffer, Mdl_);
            m_pDmaBuffer = NULL;
        }

        m_pPortStream->FreePagesFromMdl(Mdl_);
    }

    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::RegisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{
    PAGED_CODE();

    NotificationListEntry *nleNew = (NotificationListEntry*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(NotificationListEntry),
        MWBMIC_STREAM_POOLTAG);
    if (NULL == nleNew)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nleNew->NotificationEvent = NotificationEvent_;

    // Fail if the event is already registered.
    PLIST_ENTRY leCurrent = m_NotificationList.Flink;
    while (leCurrent != &m_NotificationList)
    {
        NotificationListEntry* nleCurrent = CONTAINING_RECORD(leCurrent, NotificationListEntry, ListEntry);
        if (nleCurrent->NotificationEvent == NotificationEvent_)
        {
            ExFreePoolWithTag(nleNew, MWBMIC_STREAM_POOLTAG);
            return STATUS_UNSUCCESSFUL;
        }

        leCurrent = leCurrent->Flink;
    }

    InsertTailList(&m_NotificationList, &(nleNew->ListEntry));

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::UnregisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{
    PAGED_CODE();

    PLIST_ENTRY leCurrent = m_NotificationList.Flink;
    while (leCurrent != &m_NotificationList)
    {
        NotificationListEntry* nleCurrent = CONTAINING_RECORD(leCurrent, NotificationListEntry, ListEntry);
        if (nleCurrent->NotificationEvent == NotificationEvent_)
        {
            RemoveEntryList(leCurrent);
            ExFreePoolWithTag(nleCurrent, MWBMIC_STREAM_POOLTAG);
            return STATUS_SUCCESS;
        }

        leCurrent = leCurrent->Flink;
    }

    return STATUS_NOT_FOUND;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetClockRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);

    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetPositionRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);

    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::GetHWLatency
(
    _Out_ PKSRTAUDIO_HWLATENCY  Latency_
)
{
    PAGED_CODE();

    ASSERT(Latency_);

    Latency_->ChipsetDelay = 0;
    Latency_->CodecDelay = 0;
    Latency_->FifoSize = 0;
}

//=============================================================================
#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::FreeAudioBuffer
(
    _In_opt_    PMDL        Mdl_,
    _In_        ULONG       Size_
)
{
    UNREFERENCED_PARAMETER(Size_);

    PAGED_CODE();

    if (Mdl_ != NULL)
    {
        if (m_pDmaBuffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(m_pDmaBuffer, Mdl_);
            m_pDmaBuffer = NULL;
        }

        m_pPortStream->FreePagesFromMdl(Mdl_);
    }

    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::AllocateAudioBuffer
(
    _In_    ULONG                   RequestedSize_,
    _Out_   PMDL                   *AudioBufferMdl_,
    _Out_   ULONG                  *ActualSize_,
    _Out_   ULONG                  *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE    *CacheType_
)
{
    PAGED_CODE();

    if ((0 == RequestedSize_) || (RequestedSize_ < m_ulBlockAlign))
    {
        return STATUS_UNSUCCESSFUL;
    }

    RequestedSize_ -= RequestedSize_ % (m_ulBlockAlign);

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;

    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl(highAddress, RequestedSize_);
    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    m_pDmaBuffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    if (NULL == m_pDmaBuffer)
    {
        m_pPortStream->FreePagesFromMdl(pBufferMdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_ulDmaBufferSize = RequestedSize_;
    m_ulNotificationsPerBuffer = 0;

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = RequestedSize_;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::GetPosition
(
    _Out_   KSAUDIO_POSITION    *Position_
)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(KeQueryPerformanceCounter(NULL));
    }

    Position_->PlayOffset = m_ullPlayPosition;
    Position_->WriteOffset = m_ullWritePosition;

    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    return STATUS_SUCCESS;
}

//=============================================================================
// CMiniportWaveRTStream::GetReadPacket
//
//  Returns information about the next packet for the OS to read, or
//  STATUS_DEVICE_NOT_READY if no new packet is available.
//
//  Called at PASSIVE_LEVEL but kept non-paged because it is in the streaming path.
#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CMiniportWaveRTStream::GetReadPacket
(
    _Out_ ULONG* PacketNumber,
    _Out_ DWORD* Flags,
    _Out_ ULONG64* PerformanceCounterValue,
    _Out_ BOOL* MoreData
)
{
    // Event-driven mode only.
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    *Flags = 0;

    if (m_KsState < KSSTATE_PAUSE)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    LONGLONG packetCounter = m_llPacketCounter;
    ULONGLONG ullLinearPosition = m_ullLinearPosition;
    ULONGLONG hnsElapsedTimeCarryForward = m_hnsElapsedTimeCarryForward;
    ULONGLONG ullDmaTimeStamp = m_ullDmaTimeStamp;

    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    // 0-based number of the last completed packet (ULONG_MAX during the first packet).
    ULONG availablePacketNumber = LODWORD(packetCounter - 1);

    if (availablePacketNumber == m_ulLastOsReadPacket)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    *PacketNumber = availablePacketNumber;

    // Timestamp of the end of the available packet, extrapolated from the simulated position
    // correlation [m_ullLinearPosition @ m_ullDmaTimeStamp].
    ULONGLONG linearPositionOfAvailablePacket = packetCounter * (m_ulDmaBufferSize / m_ulNotificationsPerBuffer);
    ULONGLONG carryForwardBytes = (hnsElapsedTimeCarryForward * m_ulDmaMovementRate) / 10000000;
    ULONGLONG deltaLinearPosition = ullLinearPosition + carryForwardBytes - linearPositionOfAvailablePacket;
    ULONGLONG deltaTimeInHns = deltaLinearPosition * 10000000 / m_ulDmaMovementRate;
    ULONGLONG timeOfAvailablePacketInHns = ullDmaTimeStamp - deltaTimeInHns;
    ULONGLONG timeOfAvailablePacketInQpc = timeOfAvailablePacketInHns * m_ullPerformanceCounterFrequency.QuadPart / 10000000;

    *PerformanceCounterValue = timeOfAvailablePacketInQpc;
    *MoreData = FALSE;

    m_ulLastOsReadPacket = availablePacketNumber;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
VOID CMiniportWaveRTStream::SyncPosition()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(KeQueryPerformanceCounter(NULL));
    }
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);
}

//=============================================================================
#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::SetState
(
    _In_    KSSTATE State_
)
{
    KIRQL oldIrql;

    switch (State_)
    {
        case KSSTATE_STOP:
            KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
            m_llPacketCounter = 0;
            m_ullPlayPosition = 0;
            m_ullWritePosition = 0;
            m_ullLinearPosition = 0;
            m_ulLastOsReadPacket = ULONG_MAX;
            KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

            m_pFeed->SetStreamActive(false);
            break;

        case KSSTATE_ACQUIRE:
            break;

        case KSSTATE_PAUSE:
            if (m_KsState > KSSTATE_PAUSE)
            {
                // RUN -> PAUSE: stop the simulated DMA engine.
                if (m_ulNotificationIntervalMs > 0)
                {
                    ExCancelTimer(m_pNotificationTimer, NULL);
                    KeFlushQueuedDpcs();

                    // Remember how far into the current packet we were, so the next RUN
                    // completes it at the right time.
                    if (m_ullLastDPCTimeStamp > 0)
                    {
                        LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
                        LONGLONG hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, qpc);
                        m_hnsDPCTimeCarryForward = hnsCurrentTime - m_ullLastDPCTimeStamp + m_hnsDPCTimeCarryForward;
                    }
                }

                // Deliver what is due up to now, then stop accepting feed audio.
                SyncPosition();
                m_pFeed->SetStreamActive(false);
            }
            break;

        case KSSTATE_RUN:
        {
            LARGE_INTEGER ullPerfCounterTemp = KeQueryPerformanceCounter(&m_ullPerformanceCounterFrequency);
            m_ullLastDPCTimeStamp = m_ullDmaTimeStamp = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, ullPerfCounterTemp);

            // A fresh capture session: drop anything buffered before it started.
            m_pFeed->SetStreamActive(true);

            if (m_ulNotificationIntervalMs > 0)
            {
                // 1 ms timer emulating the hardware; packets complete every notification interval.
                ExSetTimer
                (
                    m_pNotificationTimer,
                    (-1) * HNSTIME_PER_MILLISECOND,
                    HNSTIME_PER_MILLISECOND,
                    NULL
                );
            }
            break;
        }

        default:
            break;
    }

    m_KsState = State_;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::SetFormat
(
    _In_    KSDATAFORMAT    *DataFormat_
)
{
    UNREFERENCED_PARAMETER(DataFormat_);

    PAGED_CODE();

    return STATUS_NOT_SUPPORTED;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
VOID CMiniportWaveRTStream::UpdatePosition
(
    LARGE_INTEGER ilQPC
)
{
    // Convert ticks to 100ns units.
    LONGLONG hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, ilQPC);

    // Time elapsed since the last update, in ms; the sub-millisecond remainder carries forward.
    ULONGLONG hnsElapsed = hnsCurrentTime - m_ullDmaTimeStamp + m_hnsElapsedTimeCarryForward;
    ULONGLONG timeElapsedInMs = hnsElapsed / 10000;
    m_hnsElapsedTimeCarryForward = hnsElapsed % 10000;

    // Bytes the "DMA engine" moved in that time; the sub-byte remainder carries forward.
    ULONGLONG displacementTimes1000 = (ULONGLONG)m_ulDmaMovementRate * timeElapsedInMs + m_byteDisplacementCarryForward;
    ULONGLONG byteDisplacement = displacementTimes1000 / 1000;
    m_byteDisplacementCarryForward = (ULONG)(displacementTimes1000 % 1000);

    // Positions and the packet counter must advance together (GetReadPacket extrapolates
    // timestamps from both), so the full displacement is applied; the cap only guards the
    // narrowing below and is unreachable in practice (12+ hours without an update).
    if (byteDisplacement > MAXULONG)
    {
        byteDisplacement = MAXULONG;
    }
    const ULONG ByteDisplacement = (ULONG)byteDisplacement;

    WriteBytes(ByteDisplacement);

    if (m_ulDmaBufferSize > 0)
    {
        m_ullPlayPosition = m_ullWritePosition = (m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize;
    }

    m_ullLinearPosition += ByteDisplacement;
    m_ullDmaTimeStamp = hnsCurrentTime;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
VOID CMiniportWaveRTStream::WriteBytes
(
    ULONG ByteDisplacement
)
{
    if (m_pDmaBuffer == NULL || m_ulDmaBufferSize == 0)
    {
        return;
    }

    ULONG bufferOffset = (ULONG)(m_ullLinearPosition % m_ulDmaBufferSize);

    // Usually one copy, two when the run wraps around the end of the buffer.
    while (ByteDisplacement > 0)
    {
        ULONG runWrite = min(ByteDisplacement, m_ulDmaBufferSize - bufferOffset);

        m_pFeed->ReadForCapture(m_pDmaBuffer + bufferOffset, runWrite);

        bufferOffset = (bufferOffset + runWrite) % m_ulDmaBufferSize;
        ByteDisplacement -= runWrite;
    }
}

//=============================================================================
#pragma code_seg()
void
TimerNotifyRT
(
    _In_      PEX_TIMER    Timer,
    _In_opt_  PVOID        DeferredContext
)
{
    UNREFERENCED_PARAMETER(Timer);

    _IRQL_limited_to_(DISPATCH_LEVEL);

    CMiniportWaveRTStream* _this = (CMiniportWaveRTStream*)DeferredContext;
    if (NULL == _this)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&_this->m_PositionSpinLock, &oldIrql);

    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);

    // Convert ticks to 100ns units.
    LONGLONG hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(_this->m_ullPerformanceCounterFrequency.QuadPart, qpc);

    // Has a full notification interval elapsed since the last completed packet?
    ULONG TimeElapsedInMS = (ULONG)(hnsCurrentTime - _this->m_ullLastDPCTimeStamp + _this->m_hnsDPCTimeCarryForward) / 10000;

    if (TimeElapsedInMS >= _this->m_ulNotificationIntervalMs)
    {
        // Carry the excess forward so the next packet completes on time.
        _this->m_hnsDPCTimeCarryForward = hnsCurrentTime - _this->m_ullLastDPCTimeStamp + _this->m_hnsDPCTimeCarryForward - (_this->m_ulNotificationIntervalMs * 10000);
        _this->m_ullLastDPCTimeStamp = hnsCurrentTime;

        _this->UpdatePosition(qpc);
        _this->m_llPacketCounter++;

        if (_this->m_KsState == KSSTATE_RUN)
        {
            PLIST_ENTRY leCurrent = _this->m_NotificationList.Flink;
            while (leCurrent != &_this->m_NotificationList)
            {
                NotificationListEntry* nleCurrent = CONTAINING_RECORD(leCurrent, NotificationListEntry, ListEntry);
                KeSetEvent(nleCurrent->NotificationEvent, 0, 0);

                leCurrent = leCurrent->Flink;
            }
        }
    }

    KeReleaseSpinLock(&_this->m_PositionSpinLock, oldIrql);
}
