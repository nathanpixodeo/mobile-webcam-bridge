/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    endpoints.h

Abstract:

    Pin and node numbers of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: capture endpoint only, no topology nodes.
--*/
#pragma once

// Capture streams the wave filter accepts at the same time.
#define MAX_INPUT_STREAMS           1

// Wave filter pins.
enum
{
    KSPIN_WAVE_BRIDGE = 0,
    KSPIN_WAVEIN_HOST,
};

// Wave filter nodes.
enum
{
    KSNODE_WAVE_ADC = 0
};

// Topology filter pins. The topology filter has no nodes: mic -> bridge.
enum
{
    KSPIN_TOPO_MIC_ELEMENTS = 0,
    KSPIN_TOPO_BRIDGE
};
