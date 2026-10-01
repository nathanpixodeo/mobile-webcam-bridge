/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    kshelper.cpp

Abstract:

    KS property helpers.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: mixer (volume/mute/peak meter) handlers removed.
--*/

#include "definitions.h"

//-----------------------------------------------------------------------------
#pragma code_seg("PAGE")
PWAVEFORMATEX
GetWaveFormatEx
(
    _In_  PKSDATAFORMAT           pDataFormat
)
{
    PAGED_CODE();

    PWAVEFORMATEX pWfx = NULL;

    if (pDataFormat &&
        IsEqualGUIDAligned(pDataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        (IsEqualGUIDAligned(pDataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
         IsEqualGUIDAligned(pDataFormat->Specifier, KSDATAFORMAT_SPECIFIER_DSOUND)))
    {
        pWfx = PWAVEFORMATEX(pDataFormat + 1);

        if (IsEqualGUIDAligned(pDataFormat->Specifier, KSDATAFORMAT_SPECIFIER_DSOUND))
        {
            PKSDSOUND_BUFFERDESC pwfxds = PKSDSOUND_BUFFERDESC(pDataFormat + 1);
            pWfx = &pwfxds->WaveFormatEx;
        }
    }

    return pWfx;
} // GetWaveFormatEx

//-----------------------------------------------------------------------------
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_BasicSupport
(
    _In_ PPCPROPERTY_REQUEST         PropertyRequest,
    _In_ ULONG                       Flags,
    _In_ DWORD                       PropTypeSetId
)
{
    PAGED_CODE();

    ASSERT(Flags & KSPROPERTY_TYPE_BASICSUPPORT);

    NTSTATUS ntStatus = STATUS_INVALID_PARAMETER;

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        // The buffer holds a KSPROPERTY_DESCRIPTION: fill it.
        PKSPROPERTY_DESCRIPTION PropDesc = PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

        PropDesc->AccessFlags       = Flags;
        PropDesc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
        if (VT_ILLEGAL != PropTypeSetId)
        {
            PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
            PropDesc->PropTypeSet.Id    = PropTypeSetId;
        }
        else
        {
            PropDesc->PropTypeSet.Set   = GUID_NULL;
            PropDesc->PropTypeSet.Id    = 0;
        }
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount  = 0;
        PropDesc->Reserved          = 0;

        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        ntStatus = STATUS_SUCCESS;
    }
    else if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        // The buffer holds a ULONG: return the access flags only.
        *(PULONG(PropertyRequest->Value)) = Flags;

        PropertyRequest->ValueSize = sizeof(ULONG);
        ntStatus = STATUS_SUCCESS;
    }
    else
    {
        PropertyRequest->ValueSize = 0;
        ntStatus = STATUS_BUFFER_TOO_SMALL;
    }

    return ntStatus;
} // PropertyHandler_BasicSupport

#pragma code_seg()
