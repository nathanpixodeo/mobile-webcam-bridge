/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    common.h

Abstract:

    IAdapterCommon interface and endpoint description structures.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: one capture endpoint, no mixer/WDF/ETW/subdevice
    cache; the adapter exposes the microphone feed buffer instead.
--*/
#pragma once

class MicFeedBuffer;

//=============================================================================
// Signal processing modes and default formats.
//=============================================================================
typedef struct _MODE_AND_DEFAULT_FORMAT
{
    GUID            Mode;
    KSDATAFORMAT*   DefaultFormat;
} MODE_AND_DEFAULT_FORMAT, *PMODE_AND_DEFAULT_FORMAT;

typedef enum
{
    NoPin,
    BridgePin,
    SystemCapturePin,
} PINTYPE;

// Per-pin formats and modes. Arrays of this structure follow the order of the
// filter's pin descriptors, so a KS pin id is a valid index.
typedef struct _PIN_DEVICE_FORMATS_AND_MODES
{
    PINTYPE                             PinType;

    KSDATAFORMAT_WAVEFORMATEXTENSIBLE * WaveFormats;
    ULONG                               WaveFormatsCount;

    MODE_AND_DEFAULT_FORMAT *           ModeAndDefaultFormat;
    ULONG                               ModeAndDefaultFormatCount;
} PIN_DEVICE_FORMATS_AND_MODES, *PPIN_DEVICE_FORMATS_AND_MODES;

typedef struct _ENDPOINT_MINIPAIR *PENDPOINT_MINIPAIR;

// Wave and topology miniport factories share this signature.
typedef NTSTATUS (*PFNCREATEMINIPORT)(
    _Out_           PUNKNOWN              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                UnknownOuter,
    _In_            POOL_FLAGS              PoolFlags,
    _In_            PENDPOINT_MINIPAIR      MiniportPair
);

// Wave/topology filter pair making up one audio endpoint.
typedef struct _ENDPOINT_MINIPAIR
{
    // Topology miniport. TopoName must match the KSNAME_* reference string in the INF.
    PWSTR                           TopoName;
    PFNCREATEMINIPORT               TopoCreateCallback;
    PCFILTER_DESCRIPTOR*            TopoDescriptor;

    // WaveRT miniport. WaveName must match the KSNAME_* reference string in the INF.
    PWSTR                           WaveName;
    PFNCREATEMINIPORT               WaveCreateCallback;
    PCFILTER_DESCRIPTOR*            WaveDescriptor;

    USHORT                          DeviceMaxChannels;
    PIN_DEVICE_FORMATS_AND_MODES*   PinDeviceFormatsAndModes;
    ULONG                           PinDeviceFormatsAndModesCount;

    PHYSICALCONNECTIONTABLE*        PhysicalConnections;
    ULONG                           PhysicalConnectionCount;
} ENDPOINT_MINIPAIR;

//=============================================================================
// IAdapterCommon
//=============================================================================

// {74A5FB56-E038-42EB-BC1A-34C6B1A42DA2}
DEFINE_GUID(IID_IAdapterCommon,
    0x74a5fb56, 0xe038, 0x42eb, 0xbc, 0x1a, 0x34, 0xc6, 0xb1, 0xa4, 0x2d, 0xa2);

DECLARE_INTERFACE_(IAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS,        Init)
    (
        THIS_
        _In_  PDEVICE_OBJECT      DeviceObject
    ) PURE;

    STDMETHOD_(PDEVICE_OBJECT,  GetDeviceObject)
    (
        THIS
    ) PURE;

    // The PCM ring shared by the feed control device and the capture stream.
    // Valid for the lifetime of the adapter object.
    STDMETHOD_(MicFeedBuffer*,  GetMicFeedBuffer)
    (
        THIS
    ) PURE;

    STDMETHOD_(NTSTATUS,        InstallEndpointFilters)
    (
        THIS_
        _In_opt_    PIRP                Irp,
        _In_        PENDPOINT_MINIPAIR  MiniportPair
    ) PURE;
};

typedef IAdapterCommon *PADAPTERCOMMON;

//=============================================================================
// Function prototypes
//=============================================================================
NTSTATUS
NewAdapterCommon
(
    _Out_       PUNKNOWN *              Unknown,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        POOL_FLAGS              PoolFlags
);
