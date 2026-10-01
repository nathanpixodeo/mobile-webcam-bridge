/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minipairs.h

Abstract:

    The driver's single audio endpoint: the "Mobile Webcam Microphone" capture endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: render endpoint removed.
--*/
#pragma once

#include "endpoints.h"
#include "mictopo.h"
#include "mictopotable.h"
#include "micwavtable.h"

NTSTATUS
CreateMiniportWaveRT
(
    _Out_           PUNKNOWN              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                UnknownOuter,
    _In_            POOL_FLAGS              PoolFlags,
    _In_            PENDPOINT_MINIPAIR      MiniportPair
);

/*********************************************************************
* Topology/Wave bridge connection for the microphone                 *
*                                                                    *
*              +------+    +------+                                  *
*              | Topo |    | Wave |                                  *
*              |      |    |      |                                  *
*  Mic in  --->|0    1|===>|0    1|---> Capture Host Pin             *
*              |      |    |      |                                  *
*              +------+    +------+                                  *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE MicTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_BRIDGE,          // TopologyOut
        KSPIN_WAVE_BRIDGE,          // WaveIn
        CONNECTIONTYPE_TOPOLOGY_OUTPUT
    }
};

// Subdevice names: KS reference strings, must match KSNAME_* in mwbmic.inx.
static WCHAR MicTopologyName[] = L"TopologyMicMwb";
static WCHAR MicWaveName[]     = L"WaveMicMwb";

static
ENDPOINT_MINIPAIR MicMiniports =
{
    MicTopologyName,
    CreateMiniportTopologyMic,
    &MicTopoMiniportFilterDescriptor,
    MicWaveName,
    CreateMiniportWaveRT,
    &MicWaveMiniportFilterDescriptor,
    MIC_DEVICE_MAX_CHANNELS,
    MicPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicPinDeviceFormatsAndModes),
    MicTopologyPhysicalConnections,
    SIZEOF_ARRAY(MicTopologyPhysicalConnections),
};

// One endpoint = one topology miniport + one wave miniport.
#define g_MaxMiniports  2
