/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    mictopo.h

Abstract:

    Topology miniport of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL):
    basetopo.h and micarraytopo.h merged into one class.
    Modified for MobileWebcamBridge: no mixer nodes; only the jack description
    properties remain.
--*/
#pragma once

class CMiniportTopologyMic :
    public IMiniportTopology,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();

    CMiniportTopologyMic
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PCFILTER_DESCRIPTOR*    FilterDesc
    )
        : CUnknown(UnknownOuter),
          m_FilterDescriptor(FilterDesc)
    {
    }

    ~CMiniportTopologyMic();

    IMP_IMiniportTopology;

    NTSTATUS PropertyHandlerJackDescription
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

    NTSTATUS PropertyHandlerJackDescription2
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

private:
    PPCFILTER_DESCRIPTOR m_FilterDescriptor;
};

typedef CMiniportTopologyMic *PCMiniportTopologyMic;

NTSTATUS
CreateMiniportTopologyMic
(
    _Out_           PUNKNOWN              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                UnknownOuter,
    _In_            POOL_FLAGS              PoolFlags,
    _In_            PENDPOINT_MINIPAIR      MiniportPair
);

// Topology filter automation (KSPROPSETID_Jack).
NTSTATUS
PropertyHandler_MicTopoFilter
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);
