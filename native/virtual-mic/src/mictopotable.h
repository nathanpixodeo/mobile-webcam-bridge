/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    mictopotable.h

Abstract:

    Topology filter description of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL),
    micarray1toptable.h.
    Modified for MobileWebcamBridge: single microphone pin, no volume/mute nodes, no
    mic-array geometry properties.
--*/
#pragma once

// {8B92608F-CE35-4EF0-AE97-C2FBDBF095C3}
// Pin name GUID. The INF registers its display name ("Mobile Webcam Microphone") under the
// device's MediaCategories key, which becomes the endpoint name.
static const GUID MWBMIC_PIN_NAME_GUID =
    { 0x8b92608f, 0xce35, 0x4ef0, { 0xae, 0x97, 0xc2, 0xfb, 0xdb, 0xf0, 0x95, 0xc3 } };

//=============================================================================
static
KSDATARANGE MicTopoPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE MicTopoPinDataRangePointersBridge[] =
{
    &MicTopoPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicTopoMiniportPins[] =
{
    // KSPIN_TOPO_MIC_ELEMENTS
    {
        0,
        0,
        0,                                                      // InstanceCount
        NULL,                                                   // AutomationTable
        {                                                       // KsPinDescriptor
            0,                                                  // InterfacesCount
            NULL,                                               // Interfaces
            0,                                                  // MediumsCount
            NULL,                                               // Mediums
            SIZEOF_ARRAY(MicTopoPinDataRangePointersBridge),    // DataRangesCount
            MicTopoPinDataRangePointersBridge,                  // DataRanges
            KSPIN_DATAFLOW_IN,                                  // DataFlow
            KSPIN_COMMUNICATION_NONE,                           // Communication
            &KSNODETYPE_MICROPHONE,                             // Category
            &MWBMIC_PIN_NAME_GUID,                              // Name
            0                                                   // Reserved
        }
    },

    // KSPIN_TOPO_BRIDGE
    {
        0,
        0,
        0,                                                      // InstanceCount
        NULL,                                                   // AutomationTable
        {                                                       // KsPinDescriptor
            0,                                                  // InterfacesCount
            NULL,                                               // Interfaces
            0,                                                  // MediumsCount
            NULL,                                               // Mediums
            SIZEOF_ARRAY(MicTopoPinDataRangePointersBridge),    // DataRangesCount
            MicTopoPinDataRangePointersBridge,                  // DataRanges
            KSPIN_DATAFLOW_OUT,                                 // DataFlow
            KSPIN_COMMUNICATION_NONE,                           // Communication
            &KSCATEGORY_AUDIO,                                  // Category
            NULL,                                               // Name
            0                                                   // Reserved
        }
    }
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR MicTopoMiniportConnections[] =
{
    //  FromNode,       FromPin,                    ToNode,         ToPin
    {   PCFILTER_NODE,  KSPIN_TOPO_MIC_ELEMENTS,    PCFILTER_NODE,  KSPIN_TOPO_BRIDGE }
};

//=============================================================================
static
PCPROPERTY_ITEM PropertiesMicTopoFilter[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_MicTopoFilter
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_MicTopoFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicTopoFilter, PropertiesMicTopoFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicTopoMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicTopoFilter,                       // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicTopoMiniportPins),              // PinCount
    MicTopoMiniportPins,                            // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(MicTopoMiniportConnections),       // ConnectionCount
    MicTopoMiniportConnections,                     // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};
