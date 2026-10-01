/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    micwavtable.h

Abstract:

    WaveRT filter description of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL),
    micarraywavtable.h.
    Modified for MobileWebcamBridge: 48 kHz / 16-bit / mono (the feed format of
    protocol/MIC_FEED.md), default signal processing mode.
--*/
#pragma once

#include <mwb/MwbMicIoctl.h>

#define MIC_DEVICE_MAX_CHANNELS     MWBMIC_CHANNELS
#define MIC_BITS_PER_SAMPLE         MWBMIC_BITS_PER_SAMPLE
#define MIC_SAMPLE_RATE             MWBMIC_SAMPLE_RATE
#define MIC_BLOCK_ALIGN             MWBMIC_BLOCK_ALIGN
#define MIC_AVG_BYTES_PER_SEC       (MIC_SAMPLE_RATE * MIC_BLOCK_ALIGN)

C_ASSERT(MIC_BLOCK_ALIGN == MIC_DEVICE_MAX_CHANNELS * (MIC_BITS_PER_SAMPLE / 8));

//=============================================================================
// The one supported device format.
//=============================================================================
static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicPinSupportedDeviceFormats[] =
{
    // 48 kHz, 16-bit, mono
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                MIC_DEVICE_MAX_CHANNELS,
                MIC_SAMPLE_RATE,
                MIC_AVG_BYTES_PER_SEC,
                MIC_BLOCK_ALIGN,
                MIC_BITS_PER_SAMPLE,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            MIC_BITS_PER_SAMPLE,
            KSAUDIO_SPEAKER_MONO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};

//
// Supported signal processing modes (streaming pin only). The device does no processing of
// its own, so it exposes the default mode and lets the audio engine apply system effects.
//
static
MODE_AND_DEFAULT_FORMAT MicPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &MicPinSupportedDeviceFormats[0].DataFormat
    }
};

//
// Same order as the filter's pin descriptor array.
//
static
PIN_DEVICE_FORMATS_AND_MODES MicPinDeviceFormatsAndModes[] =
{
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    },
    {
        SystemCapturePin,
        MicPinSupportedDeviceFormats,
        SIZEOF_ARRAY(MicPinSupportedDeviceFormats),
        MicPinSupportedDeviceModes,
        SIZEOF_ARRAY(MicPinSupportedDeviceModes)
    }
};

//=============================================================================
// Data ranges
//=============================================================================

// The streaming pin's data range carries the signal processing mode attribute.
static
KSATTRIBUTE PinDataRangeSignalProcessingModeAttribute =
{
    sizeof(KSATTRIBUTE),
    0,
    STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE),
};

static
PKSATTRIBUTE PinDataRangeAttributes[] =
{
    &PinDataRangeSignalProcessingModeAttribute,
};

static
KSATTRIBUTE_LIST PinDataRangeAttributeList =
{
    ARRAYSIZE(PinDataRangeAttributes),
    PinDataRangeAttributes,
};

static
KSDATARANGE_AUDIO MicPinDataRangesStream[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        MIC_DEVICE_MAX_CHANNELS,
        MIC_BITS_PER_SAMPLE,
        MIC_BITS_PER_SAMPLE,
        MIC_SAMPLE_RATE,
        MIC_SAMPLE_RATE
    },
};

static
PKSDATARANGE MicPinDataRangePointersStream[] =
{
    PKSDATARANGE(&MicPinDataRangesStream[0]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

static
KSDATARANGE MicPinDataRangesBridge[] =
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
PKSDATARANGE MicPinDataRangePointersBridge[] =
{
    &MicPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicWaveMiniportPins[] =
{
    // KSPIN_WAVE_BRIDGE: capture input from the topology filter
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicPinDataRangePointersBridge),
            MicPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // KSPIN_WAVEIN_HOST: capture streaming pin
    {
        MAX_INPUT_STREAMS,
        MAX_INPUT_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicPinDataRangePointersStream),
            MicPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    }
};

//=============================================================================
static
PCNODE_DESCRIPTOR MicWaveMiniportNodes[] =
{
    // KSNODE_WAVE_ADC
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_ADC,        // Type
        NULL                    // Name
    }
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR MicWaveMiniportConnections[] =
{
    { PCFILTER_NODE,        KSPIN_WAVE_BRIDGE,      KSNODE_WAVE_ADC,     1 },
    { KSNODE_WAVE_ADC,      0,                      PCFILTER_NODE,       KSPIN_WAVEIN_HOST },
};

//=============================================================================
static
PCPROPERTY_ITEM PropertiesMicWaveFilter[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicWaveFilter, PropertiesMicWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicWaveFilter,                       // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicWaveMiniportPins),              // PinCount
    MicWaveMiniportPins,                            // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    SIZEOF_ARRAY(MicWaveMiniportNodes),             // NodeCount
    MicWaveMiniportNodes,                           // Nodes
    SIZEOF_ARRAY(MicWaveMiniportConnections),       // ConnectionCount
    MicWaveMiniportConnections,                     // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories: audio, capture (defaults)
};
