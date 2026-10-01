/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavert.cpp

Abstract:

    Implementation of the WaveRT miniport of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: capture only; the client's data format size is
    validated before it is read; the format and mode tables are static, so the
    sample's table spinlock is gone.
--*/

#include "definitions.h"
#include "endpoints.h"
#include "minwavert.h"
#include "minwavertstream.h"

//=============================================================================
// CMiniportWaveRT
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
CreateMiniportWaveRT
(
    _Out_           PUNKNOWN              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                UnknownOuter,
    _In_            POOL_FLAGS              PoolFlags,
    _In_            PENDPOINT_MINIPAIR      MiniportPair
)
{
    PAGED_CODE();

    ASSERT(Unknown);
    ASSERT(MiniportPair);

    CMiniportWaveRT *obj = new (PoolFlags, MWBMIC_WAVERT_POOLTAG) CMiniportWaveRT(UnknownOuter, MiniportPair);
    if (NULL == obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();
    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRT::CMiniportWaveRT
(
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        PENDPOINT_MINIPAIR      MiniportPair
)
    : CUnknown(UnknownOuter),
      m_ulSystemAllocated(0),
      m_ulMaxSystemStreams(0),
      m_pMiniportPair(MiniportPair),
      m_pAdapterCommon(NULL)
{
    PAGED_CODE();

    RtlCopyMemory(&m_FilterDesc, MiniportPair->WaveDescriptor, sizeof(m_FilterDesc));

    // The capture bridge pin comes first in the pin enumeration.
    if (m_FilterDesc.PinCount > KSPIN_WAVEIN_HOST)
    {
        m_ulMaxSystemStreams = m_FilterDesc.Pins[KSPIN_WAVEIN_HOST].MaxFilterInstanceCount;
    }
}

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRT::~CMiniportWaveRT
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::~CMiniportWaveRT]"));

    SAFE_RELEASE(m_pAdapterCommon);
} // ~CMiniportWaveRT

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection
(
    _In_        ULONG                       PinId,
    _In_        PKSDATARANGE                ClientDataRange,
    _In_        PKSDATARANGE                MyDataRange,
    _In_        ULONG                       OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                       ResultantFormat,
    _Out_       PULONG                      ResultantFormatLength
)
/*++

Routine Description:

  The streaming pin supports exactly one format, so the intersection is that
  format whenever the client's range admits it.

--*/
{
    UNREFERENCED_PARAMETER(MyDataRange);

    PAGED_CODE();

    if (!IsEqualGUIDAligned(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        !IsSystemCapturePin(PinId))
    {
        // Let the class handler deal with everything else.
        return STATUS_NOT_IMPLEMENTED;
    }

    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(PinId);
    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE* deviceFormat = &pinInfo->WaveFormats[0];

    if (ClientDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
    {
        const KSDATARANGE_AUDIO* clientRange = reinterpret_cast<const KSDATARANGE_AUDIO*>(ClientDataRange);
        const WAVEFORMATEX& wfx = deviceFormat->WaveFormatExt.Format;

        if (clientRange->MaximumChannels < wfx.nChannels ||
            clientRange->MinimumBitsPerSample > wfx.wBitsPerSample ||
            clientRange->MaximumBitsPerSample < wfx.wBitsPerSample ||
            clientRange->MinimumSampleFrequency > wfx.nSamplesPerSec ||
            clientRange->MaximumSampleFrequency < wfx.nSamplesPerSec)
        {
            return STATUS_NO_MATCH;
        }
    }

    const ULONG requiredSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    // A size query is answered before any other error.
    if (!OutputBufferLength)
    {
        *ResultantFormatLength = requiredSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < requiredSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    *static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(ResultantFormat) = *deviceFormat;
    *ResultantFormatLength = requiredSize;

    return STATUS_SUCCESS;
} // DataRangeIntersection

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR * OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    *OutFilterDescriptor = &m_FilterDesc;

    return STATUS_SUCCESS;
} // GetDescription

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init
(
    _In_  PUNKNOWN                UnknownAdapter_,
    _In_  PRESOURCELIST           ResourceList_,
    _In_  PPORTWAVERT             Port_
)
{
    UNREFERENCED_PARAMETER(ResourceList_);
    UNREFERENCED_PARAMETER(Port_);

    PAGED_CODE();

    ASSERT(UnknownAdapter_);

    DPF_ENTER(("[CMiniportWaveRT::Init]"));

    if (m_ulMaxSystemStreams == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    // Keep the adapter (and with it the feed buffer) alive as long as this miniport and its
    // streams exist.
    return UnknownAdapter_->QueryInterface(IID_IAdapterCommon, (PVOID *)&m_pAdapterCommon);
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream
(
    _Out_ PMINIPORTWAVERTSTREAM * OutStream,
    _In_  PPORTWAVERTSTREAM       OuterUnknown,
    _In_  ULONG                   Pin,
    _In_  BOOLEAN                 Capture,
    _In_  PKSDATAFORMAT           DataFormat
)
{
    PAGED_CODE();

    ASSERT(OutStream);
    ASSERT(DataFormat);

    DPF_ENTER(("[CMiniportWaveRT::NewStream]"));

    NTSTATUS                    ntStatus = STATUS_SUCCESS;
    PCMiniportWaveRTStream      stream = NULL;
    GUID                        signalProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;

    *OutStream = NULL;

    // Extract the attributes (QWORD aligned after the data format), if any.
    if (DataFormat->Flags & KSDATAFORMAT_ATTRIBUTES)
    {
        PKSMULTIPLE_ITEM attributes = (PKSMULTIPLE_ITEM) (((PBYTE)DataFormat) + ((DataFormat->FormatSize + FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT));
        ntStatus = GetAttributesFromAttributeList(attributes, attributes->Size, &signalProcessingMode);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = ValidateStreamCreate(Pin, Capture);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = IsFormatSupported(Pin, DataFormat);
    }

    // The stream is touched from a DPC: non-paged.
    if (NT_SUCCESS(ntStatus))
    {
        stream = new (POOL_FLAG_NON_PAGED, MWBMIC_STREAM_POOLTAG) CMiniportWaveRTStream(NULL);

        if (stream)
        {
            stream->AddRef();

            ntStatus = stream->Init(this, OuterUnknown, Pin, DataFormat, signalProcessingMode);
        }
        else
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
        *OutStream = PMINIPORTWAVERTSTREAM(stream);
        (*OutStream)->AddRef();
    }

    // Our private reference; the caller holds its own.
    if (stream)
    {
        stream->Release();
    }

    return ntStatus;
} // NewStream

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
    {
        *Object = PVOID(PMINIPORTWAVERT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportAudioSignalProcessing))
    {
        *Object = PVOID(PMINIPORTAudioSignalProcessing(this));
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
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDeviceDescription
(
    _Out_ PDEVICE_DESCRIPTION DmaDeviceDescription
)
{
    PAGED_CODE();

    ASSERT(DmaDeviceDescription);

    DPF_ENTER(("[CMiniportWaveRT::GetDeviceDescription]"));

    // Virtual device: the description is never used for real DMA.
    RtlZeroMemory(DmaDeviceDescription, sizeof(DEVICE_DESCRIPTION));

    DmaDeviceDescription->Master = TRUE;
    DmaDeviceDescription->ScatterGather = TRUE;
    DmaDeviceDescription->Dma32BitAddresses = TRUE;
    DmaDeviceDescription->InterfaceType = PCIBus;
    DmaDeviceDescription->MaximumLength = 0xFFFFFFFF;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::GetModes
(
    _In_                                        ULONG     Pin,
    _Out_writes_opt_(*NumSignalProcessingModes) GUID*     SignalProcessingModes,
    _Inout_                                     ULONG*    NumSignalProcessingModes
)
/*++

  Returns STATUS_INVALID_PARAMETER for an unknown pin, STATUS_NOT_SUPPORTED for
  a pin without modes (the bridge pin), the modes otherwise.

--*/
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::GetModes]"));

    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(Pin);
    if (pinInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONG numModes = pinInfo->ModeAndDefaultFormatCount;
    if (numModes == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (SignalProcessingModes != NULL)
    {
        if (*NumSignalProcessingModes < numModes)
        {
            *NumSignalProcessingModes = numModes;
            return STATUS_BUFFER_TOO_SMALL;
        }

        for (ULONG i = 0; i < numModes; ++i)
        {
            SignalProcessingModes[i] = pinInfo->ModeAndDefaultFormat[i].Mode;
        }
    }

    *NumSignalProcessingModes = numModes;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::ValidateStreamCreate
(
    _In_    ULONG   Pin,
    _In_    BOOLEAN Capture
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::ValidateStreamCreate]"));

    NTSTATUS ntStatus = STATUS_NOT_SUPPORTED;

    if (Capture && IsSystemCapturePin(Pin))
    {
        VERIFY_PIN_INSTANCE_RESOURCES_AVAILABLE(ntStatus, m_ulSystemAllocated, m_ulMaxSystemStreams);
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::StreamCreated
(
    _In_ ULONG Pin
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::StreamCreated]"));

    if (IsSystemCapturePin(Pin))
    {
        ALLOCATE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::StreamClosed
(
    _In_ ULONG Pin
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::StreamClosed]"));

    if (IsSystemCapturePin(Pin))
    {
        FREE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::GetAttributesFromAttributeList
(
    _In_ const KSMULTIPLE_ITEM *Attributes,
    _In_ size_t Size,
    _Out_ GUID* SignalProcessingMode
)
/*++

Routine Description:

  Walks an attribute list and returns the signal processing mode it carries,
  or AUDIO_SIGNALPROCESSINGMODE_DEFAULT when absent. Every read is bounds
  checked against Size.

--*/
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::GetAttributesFromAttributeList]"));

    size_t cbRemaining = Size;

    *SignalProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;

    if (cbRemaining < sizeof(KSMULTIPLE_ITEM))
    {
        return STATUS_INVALID_PARAMETER;
    }
    cbRemaining -= sizeof(KSMULTIPLE_ITEM);

    PKSATTRIBUTE attributeHeader = (PKSATTRIBUTE)(Attributes + 1);

    for (ULONG i = 0; i < Attributes->Count; i++)
    {
        if (cbRemaining < sizeof(KSATTRIBUTE))
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (attributeHeader->Attribute == KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE)
        {
            if (cbRemaining < sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE) ||
                attributeHeader->Size != sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE))
            {
                return STATUS_INVALID_PARAMETER;
            }

            KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE* modeAttribute =
                (KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE*)attributeHeader;

            *SignalProcessingMode = modeAttribute->SignalProcessingMode;
        }
        else
        {
            return STATUS_NOT_SUPPORTED;
        }

        // Next attribute (QWORD aligned).
        ULONG cbAttribute = ((attributeHeader->Size + FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT);
        if (cbAttribute > cbRemaining)
        {
            cbRemaining = 0;
        }
        else
        {
            cbRemaining -= cbAttribute;
        }
        attributeHeader = (PKSATTRIBUTE) (((PBYTE)attributeHeader) + cbAttribute);
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::IsFormatSupported
(
    _In_ ULONG          PinId,
    _In_ PKSDATAFORMAT  DataFormat
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::IsFormatSupported]"));

    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(PinId);
    if (pinInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Never read past the client's buffer.
    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
    {
        return STATUS_NO_MATCH;
    }

    PWAVEFORMATEX pWaveFormat = reinterpret_cast<PWAVEFORMATEX>(DataFormat + 1);

    for (ULONG iFormat = 0; iFormat < pinInfo->WaveFormatsCount; iFormat++)
    {
        const KSDATAFORMAT_WAVEFORMATEXTENSIBLE* pFormat = &pinInfo->WaveFormats[iFormat];

        // KSDATAFORMAT
        if (!IsEqualGUIDAligned(pFormat->DataFormat.MajorFormat, DataFormat->MajorFormat)) { continue; }
        if (!IsEqualGUIDAligned(pFormat->DataFormat.SubFormat, DataFormat->SubFormat)) { continue; }
        if (!IsEqualGUIDAligned(pFormat->DataFormat.Specifier, DataFormat->Specifier)) { continue; }

        // WAVEFORMATEX
        if (pWaveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        {
            if (pWaveFormat->wFormatTag != EXTRACT_WAVEFORMATEX_ID(&(pFormat->WaveFormatExt.SubFormat))) { continue; }
        }
        if (pWaveFormat->nChannels      != pFormat->WaveFormatExt.Format.nChannels) { continue; }
        if (pWaveFormat->nSamplesPerSec != pFormat->WaveFormatExt.Format.nSamplesPerSec) { continue; }
        if (pWaveFormat->nBlockAlign    != pFormat->WaveFormatExt.Format.nBlockAlign) { continue; }
        if (pWaveFormat->wBitsPerSample != pFormat->WaveFormatExt.Format.wBitsPerSample) { continue; }
        if (pWaveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        {
            return STATUS_SUCCESS;
        }

        // WAVEFORMATEXTENSIBLE
        if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) { continue; }
        if (pWaveFormat->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) { continue; }

        PWAVEFORMATEXTENSIBLE pWaveFormatExt = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pWaveFormat);
        if (pWaveFormatExt->Samples.wValidBitsPerSample != pFormat->WaveFormatExt.Samples.wValidBitsPerSample) { continue; }
        if (pWaveFormatExt->dwChannelMask != pFormat->WaveFormatExt.dwChannelMask) { continue; }
        if (!IsEqualGUIDAligned(pWaveFormatExt->SubFormat, pFormat->WaveFormatExt.SubFormat)) { continue; }

        return STATUS_SUCCESS;
    }

    return STATUS_NO_MATCH;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::PropertyHandlerProposedFormat]"));

    // Every request carries a KSP_PIN.
    if (PropertyRequest->InstanceSize < (sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    PKSP_PIN kspPin = CONTAINING_RECORD(PropertyRequest->Instance, KSP_PIN, PinId);

    // Valid on the streaming pin only.
    if (IsBridgePin(kspPin->PinId))
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (!IsSystemCapturePin(kspPin->PinId))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest, PropertyRequest->PropertyItem->Flags, VT_ILLEGAL);
    }

    const ULONG cbMinSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbMinSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        return IsFormatSupported(kspPin->PinId, (PKSDATAFORMAT)PropertyRequest->Value);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
} // PropertyHandlerProposedFormat

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat2
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::PropertyHandlerProposedFormat2]"));

    if (PropertyRequest->InstanceSize < (sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    PKSP_PIN kspPin = CONTAINING_RECORD(PropertyRequest->Instance, KSP_PIN, PinId);

    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(kspPin->PinId);
    if (pinInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Supported only on pins with modes that carry a default format.
    const MODE_AND_DEFAULT_FORMAT* modes = pinInfo->ModeAndDefaultFormat;
    const ULONG numModes = pinInfo->ModeAndDefaultFormatCount;

    BOOLEAN anyDefaultFormat = FALSE;
    for (ULONG i = 0; i < numModes; ++i)
    {
        if (modes[i].DefaultFormat != NULL)
        {
            anyDefaultFormat = TRUE;
            break;
        }
    }
    if (!anyDefaultFormat)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest, PropertyRequest->PropertyItem->Flags, VT_ILLEGAL);
    }

    // The instance data after KSP_PIN is the attribute list naming the mode.
    PKSMULTIPLE_ITEM pKsItemsHeader = (PKSMULTIPLE_ITEM)(kspPin + 1);
    size_t cbItemsList = (((PBYTE)PropertyRequest->Instance) + PropertyRequest->InstanceSize) - (PBYTE)pKsItemsHeader;

    GUID signalProcessingMode = {0};
    NTSTATUS ntStatus = GetAttributesFromAttributeList(pKsItemsHeader, cbItemsList, &signalProcessingMode);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    const MODE_AND_DEFAULT_FORMAT* modeInfo = NULL;
    for (ULONG i = 0; i < numModes; ++i)
    {
        if (modes[i].Mode == signalProcessingMode)
        {
            modeInfo = &modes[i];
            break;
        }
    }

    if (modeInfo == NULL || modeInfo->DefaultFormat == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    // Output: default format, QWORD aligned, followed by the caller's attribute list.
    ULONG cbMinSize = modeInfo->DefaultFormat->FormatSize;
    cbMinSize = (cbMinSize + 7) & ~7;

    PKSMULTIPLE_ITEM pKsItemsHeaderOut = (PKSMULTIPLE_ITEM)((PBYTE)PropertyRequest->Value + cbMinSize);

    if (cbItemsList > MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ntStatus = RtlULongAdd(cbMinSize, (ULONG)cbItemsList, &cbMinSize);
    if (!NT_SUCCESS(ntStatus))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbMinSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if ((PropertyRequest->Verb & KSPROPERTY_TYPE_GET) == 0)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    RtlCopyMemory(PropertyRequest->Value, modeInfo->DefaultFormat, modeInfo->DefaultFormat->FormatSize);

    ASSERT(cbItemsList > 0);
    ((KSDATAFORMAT*)PropertyRequest->Value)->Flags = KSDATAFORMAT_ATTRIBUTES;
    RtlCopyMemory(pKsItemsHeaderOut, pKsItemsHeader, cbItemsList);

    PropertyRequest->ValueSize = cbMinSize;

    return STATUS_SUCCESS;
} // PropertyHandlerProposedFormat2

//=============================================================================
#pragma code_seg()
const PIN_DEVICE_FORMATS_AND_MODES*
CMiniportWaveRT::GetPinInfo
(
    _In_ ULONG PinId
) const
{
    if (PinId >= m_pMiniportPair->PinDeviceFormatsAndModesCount)
    {
        return NULL;
    }

    return &m_pMiniportPair->PinDeviceFormatsAndModes[PinId];
}

#pragma code_seg()
BOOL
CMiniportWaveRT::IsSystemCapturePin
(
    _In_ ULONG PinId
) const
{
    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(PinId);
    return pinInfo != NULL && pinInfo->PinType == SystemCapturePin && pinInfo->WaveFormatsCount > 0;
}

#pragma code_seg()
BOOL
CMiniportWaveRT::IsBridgePin
(
    _In_ ULONG PinId
) const
{
    const PIN_DEVICE_FORMATS_AND_MODES* pinInfo = GetPinInfo(PinId);
    return pinInfo != NULL && pinInfo->PinType == BridgePin;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_WaveFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Redirects wave filter property requests to the miniport object.

--*/
{
    PAGED_CODE();

    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    CMiniportWaveRT*    pWaveHelper = reinterpret_cast<CMiniportWaveRT*>(PropertyRequest->MajorTarget);

    if (pWaveHelper == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pWaveHelper->AddRef();

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Pin))
    {
        switch (PropertyRequest->PropertyItem->Id)
        {
            case KSPROPERTY_PIN_PROPOSEDATAFORMAT:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat(PropertyRequest);
                break;

            case KSPROPERTY_PIN_PROPOSEDATAFORMAT2:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat2(PropertyRequest);
                break;

            default:
                DPF(D_TERSE, ("[PropertyHandler_WaveFilter: Invalid Device Request]"));
                break;
        }
    }

    pWaveHelper->Release();

    return ntStatus;
} // PropertyHandler_WaveFilter

#pragma code_seg()
