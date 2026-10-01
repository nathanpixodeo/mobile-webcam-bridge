/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavert.h

Abstract:

    WaveRT miniport of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: capture only (render, loopback, offload, DRM and
    pin-caps events removed); holds a strong reference to the adapter object.
--*/
#pragma once

class CMiniportWaveRTStream;

class CMiniportWaveRT :
    public IMiniportWaveRT,
    public IMiniportAudioSignalProcessing,
    public CUnknown
{
private:
    ULONG                               m_ulSystemAllocated;
    ULONG                               m_ulMaxSystemStreams;
    PCFILTER_DESCRIPTOR                 m_FilterDesc;
    PENDPOINT_MINIPAIR                  m_pMiniportPair;
    PADAPTERCOMMON                      m_pAdapterCommon;   // strong reference, taken in Init

public:
    DECLARE_STD_UNKNOWN();

    CMiniportWaveRT
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PENDPOINT_MINIPAIR      MiniportPair
    );

    ~CMiniportWaveRT();

    IMP_IMiniportWaveRT;
    IMP_IMiniportAudioSignalProcessing;

    // Friends
    friend class CMiniportWaveRTStream;
    friend NTSTATUS PropertyHandler_WaveFilter
    (
        _In_ PPCPROPERTY_REQUEST PropertyRequest
    );

    // Valid after Init succeeded and for the miniport's lifetime.
    PADAPTERCOMMON GetAdapterCommon() const
    {
        return m_pAdapterCommon;
    }

private:
    NTSTATUS ValidateStreamCreate
    (
        _In_    ULONG   Pin,
        _In_    BOOLEAN Capture
    );

    NTSTATUS StreamCreated
    (
        _In_ ULONG Pin
    );

    NTSTATUS StreamClosed
    (
        _In_ ULONG Pin
    );

    NTSTATUS IsFormatSupported
    (
        _In_ ULONG          PinId,
        _In_ PKSDATAFORMAT  DataFormat
    );

    static NTSTATUS GetAttributesFromAttributeList
    (
        _In_ const KSMULTIPLE_ITEM *Attributes,
        _In_ size_t                 Size,
        _Out_ GUID*                 SignalProcessingMode
    );

    NTSTATUS PropertyHandlerProposedFormat
    (
        _In_ PPCPROPERTY_REQUEST PropertyRequest
    );

    NTSTATUS PropertyHandlerProposedFormat2
    (
        _In_ PPCPROPERTY_REQUEST PropertyRequest
    );

    const PIN_DEVICE_FORMATS_AND_MODES* GetPinInfo(_In_ ULONG PinId) const;
    BOOL IsSystemCapturePin(_In_ ULONG PinId) const;
    BOOL IsBridgePin(_In_ ULONG PinId) const;
};

typedef CMiniportWaveRT *PCMiniportWaveRT;
