/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    kshelper.h

Abstract:

    KS property helpers.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: only the helpers this driver uses remain.
--*/
#pragma once

// Returns the WAVEFORMATEX inside a known audio data format, or NULL.
PWAVEFORMATEX
GetWaveFormatEx
(
    _In_  PKSDATAFORMAT           pDataFormat
);

// Default KSPROPERTY_TYPE_BASICSUPPORT handler: returns the access flags
// (ULONG) or a KSPROPERTY_DESCRIPTION, depending on the caller's buffer size.
NTSTATUS
PropertyHandler_BasicSupport
(
    _In_  PPCPROPERTY_REQUEST     PropertyRequest,
    _In_  ULONG                   Flags,
    _In_  DWORD                   PropTypeSetId
);
