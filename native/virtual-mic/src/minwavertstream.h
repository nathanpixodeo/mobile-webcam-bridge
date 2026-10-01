/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavertstream.h

Abstract:

    WaveRT capture stream of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: capture only; the simulated DMA engine copies
    PCM from the adapter's MicFeedBuffer instead of generating a sine tone.
--*/
#pragma once

class MicFeedBuffer;

// Notification event registered by the audio engine (event-driven mode).
typedef struct _NotificationListEntry
{
    LIST_ENTRY  ListEntry;
    PKEVENT     NotificationEvent;
} NotificationListEntry;

EXT_CALLBACK   TimerNotifyRT;

class CMiniportWaveRT;

class CMiniportWaveRTStream :
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTInputStream,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTInputStream;

    NTSTATUS Init
    (
        _In_  CMiniportWaveRT*    Miniport,
        _In_  PPORTWAVERTSTREAM   Stream,
        _In_  ULONG               Pin,
        _In_  PKSDATAFORMAT       DataFormat,
        _In_  GUID                SignalProcessingMode
    );

    friend EXT_CALLBACK TimerNotifyRT;

private:
    PPORTWAVERTSTREAM           m_pPortStream;
    LIST_ENTRY                  m_NotificationList;
    PEX_TIMER                   m_pNotificationTimer;
    ULONG                       m_ulNotificationIntervalMs;

    CMiniportWaveRT*            m_pMiniport;
    MicFeedBuffer*              m_pFeed;            // owned by the adapter, kept alive via m_pMiniport
    ULONG                       m_ulPin;
    BOOLEAN                     m_bUnregisterStream;
    ULONG                       m_ulDmaBufferSize;
    BYTE*                       m_pDmaBuffer;
    ULONG                       m_ulNotificationsPerBuffer;
    KSSTATE                     m_KsState;
    ULONGLONG                   m_ullPlayPosition;
    ULONGLONG                   m_ullWritePosition;
    ULONGLONG                   m_ullLinearPosition;
    ULONG                       m_ulLastOsReadPacket;
    LONGLONG                    m_llPacketCounter;
    ULONGLONG                   m_ullDmaTimeStamp;
    LARGE_INTEGER               m_ullPerformanceCounterFrequency;
    ULONGLONG                   m_hnsElapsedTimeCarryForward;
    ULONGLONG                   m_ullLastDPCTimeStamp;
    ULONGLONG                   m_hnsDPCTimeCarryForward;
    ULONG                       m_byteDisplacementCarryForward;
    ULONG                       m_ulDmaMovementRate;
    ULONG                       m_ulBlockAlign;     // bytes per audio frame of the stream format
    GUID                        m_SignalProcessingMode;
    KSPIN_LOCK                  m_PositionSpinLock;

    // Copies `ByteDisplacement` bytes of microphone audio into the DMA buffer at the current
    // linear position.
    _IRQL_requires_(DISPATCH_LEVEL)
    _Requires_lock_held_(m_PositionSpinLock)
    VOID WriteBytes
    (
        _In_ ULONG ByteDisplacement
    );

    // Advances the simulated DMA position to `ilQPC`, filling the buffer on the way.
    _IRQL_requires_(DISPATCH_LEVEL)
    _Requires_lock_held_(m_PositionSpinLock)
    VOID UpdatePosition
    (
        _In_ LARGE_INTEGER ilQPC
    );

    // Brings the linear position up to date (used on RUN -> PAUSE).
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID SyncPosition();
};

typedef CMiniportWaveRTStream *PCMiniportWaveRTStream;
