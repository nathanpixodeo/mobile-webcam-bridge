/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    mictopo.cpp

Abstract:

    Topology miniport of the microphone endpoint.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL):
    basetopo.cpp and micarraytopo.cpp merged.
    Modified for MobileWebcamBridge: no mixer nodes, no mic-array geometry; the jack
    is reported as always connected.
--*/

#include "definitions.h"
#include "endpoints.h"
#include "mictopo.h"

#pragma code_seg("PAGE")

//=============================================================================
NTSTATUS
CreateMiniportTopologyMic
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

    CMiniportTopologyMic* obj =
        new (PoolFlags, MWBMIC_TOPOLOGY_POOLTAG) CMiniportTopologyMic(UnknownOuter, MiniportPair->TopoDescriptor);
    if (obj == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();
    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
} // CreateMiniportTopologyMic

//=============================================================================
CMiniportTopologyMic::~CMiniportTopologyMic
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportTopologyMic::~CMiniportTopologyMic]"));
} // ~CMiniportTopologyMic

//=============================================================================
STDMETHODIMP
CMiniportTopologyMic::DataRangeIntersection
(
    _In_        ULONG                   PinId,
    _In_        PKSDATARANGE            ClientDataRange,
    _In_        PKSDATARANGE            MyDataRange,
    _In_        ULONG                   OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                   ResultantFormat     OPTIONAL,
    _Out_       PULONG                  ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);

    PAGED_CODE();

    // Topology pins carry analog bridge ranges only; let the class handler decide.
    return STATUS_NOT_IMPLEMENTED;
} // DataRangeIntersection

//=============================================================================
STDMETHODIMP
CMiniportTopologyMic::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR* OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    *OutFilterDescriptor = m_FilterDescriptor;

    return STATUS_SUCCESS;
} // GetDescription

//=============================================================================
STDMETHODIMP
CMiniportTopologyMic::Init
(
    _In_ PUNKNOWN                 UnknownAdapter,
    _In_ PRESOURCELIST            ResourceList,
    _In_ PPORTTOPOLOGY            Port_
)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port_);

    PAGED_CODE();

    DPF_ENTER(("[CMiniportTopologyMic::Init]"));

    // Nothing to initialise: the topology has no controls and raises no events.
    return STATUS_SUCCESS;
} // Init

//=============================================================================
STDMETHODIMP
CMiniportTopologyMic::NonDelegatingQueryInterface
(
    _In_         REFIID                  Interface,
    _COM_Outptr_ PVOID*                  Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
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
NTSTATUS
CMiniportTopologyMic::PropertyHandlerJackDescription
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Handles ( KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION ). The virtual device
  has no presence detection, so the jack is always reported as connected.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    if (PropertyRequest->InstanceSize < sizeof(ULONG) ||
        *(PULONG(PropertyRequest->Instance)) != KSPIN_TOPO_MIC_ELEMENTS)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest,
                                            KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                                            VT_ILLEGAL);
    }

    const ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbNeeded;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbNeeded)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!(PropertyRequest->Verb & KSPROPERTY_TYPE_GET))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
    PKSJACK_DESCRIPTION pDesc = (PKSJACK_DESCRIPTION)(pMI + 1);

    pMI->Size = cbNeeded;
    pMI->Count = 1;

    pDesc->ChannelMapping = KSAUDIO_SPEAKER_MONO;
    pDesc->Color = 0x00000000;
    pDesc->ConnectionType = eConnTypeUnknown;
    pDesc->GenLocation = eGenLocPrimaryBox;
    pDesc->GeoLocation = eGeoLocFront;
    pDesc->PortConnection = ePortConnIntegratedDevice;
    pDesc->IsConnected = TRUE;

    PropertyRequest->ValueSize = cbNeeded;
    return STATUS_SUCCESS;
}

//=============================================================================
NTSTATUS
CMiniportTopologyMic::PropertyHandlerJackDescription2
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Handles ( KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2 ): no presence
  detection, no dynamic format change.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    if (PropertyRequest->InstanceSize < sizeof(ULONG) ||
        *(PULONG(PropertyRequest->Instance)) != KSPIN_TOPO_MIC_ELEMENTS)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest,
                                            KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                                            VT_ILLEGAL);
    }

    const ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbNeeded;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbNeeded)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!(PropertyRequest->Verb & KSPROPERTY_TYPE_GET))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
    PKSJACK_DESCRIPTION2 pDesc = (PKSJACK_DESCRIPTION2)(pMI + 1);

    pMI->Size = cbNeeded;
    pMI->Count = 1;

    RtlZeroMemory(pDesc, sizeof(KSJACK_DESCRIPTION2));
    pDesc->DeviceStateInfo = 0;
    pDesc->JackCapabilities = 0;

    PropertyRequest->ValueSize = cbNeeded;
    return STATUS_SUCCESS;
}

//=============================================================================
NTSTATUS
PropertyHandler_MicTopoFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Redirects topology filter property requests to the miniport object
  (PortCls sets MajorTarget to the miniport).

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    PCMiniportTopologyMic pMiniport = (PCMiniportTopologyMic)PropertyRequest->MajorTarget;
    if (pMiniport == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            return pMiniport->PropertyHandlerJackDescription(PropertyRequest);
        }
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            return pMiniport->PropertyHandlerJackDescription2(PropertyRequest);
        }
    }

    return STATUS_INVALID_DEVICE_REQUEST;
} // PropertyHandler_MicTopoFilter

#pragma code_seg()
