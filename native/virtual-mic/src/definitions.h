/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    definitions.h

Abstract:

    Common includes, pool tags and helper macros for the MobileWebcamBridge virtual
    microphone driver.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: WDF, mixer, DRM and render definitions removed.
--*/
#pragma once

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <ntintsafe.h>
#include "NewDelete.h"

//=============================================================================
// Pool tags (poolmon shows them reversed: IpMa, IpMw, IpMs, IpMt, IpMf)
//=============================================================================
#define MWBMIC_ADAPTER_POOLTAG      'aMpI'
#define MWBMIC_WAVERT_POOLTAG       'wMpI'
#define MWBMIC_STREAM_POOLTAG       'sMpI'
#define MWBMIC_TOPOLOGY_POOLTAG     'tMpI'
#define MWBMIC_FEED_POOLTAG         'fMpI'

//=============================================================================
// Debug output (ksdebug.h, level selected by DEBUG_LEVEL)
//
// _DbgPrintF compares constant levels (C4127 at /W4) and, in DBG builds, breaks
// into the debugger for DEBUGLVL_ERROR. This driver therefore traces failures
// at D_TERSE so a debug build without an attached debugger does not bugcheck.
//=============================================================================
#pragma warning(disable : 4127)

#define STR_MODULENAME              "MobileWebcamBridgeMic: "
#define D_FUNC                      4
#define D_VERBOSE                   DEBUGLVL_VERBOSE
#define D_TERSE                     DEBUGLVL_TERSE
#define D_ERROR                     DEBUGLVL_ERROR
#define DPF                         _DbgPrintF
#define DPF_ENTER(x)                DPF(D_FUNC, x)

#define HNSTIME_PER_MILLISECOND     10000

//=============================================================================
// Helpers kept from the sample
//=============================================================================
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

#define VERIFY_PIN_INSTANCE_RESOURCES_AVAILABLE(status, allocated, max) \
    status = ((allocated) < (max)) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES

#define ALLOCATE_PIN_INSTANCE_RESOURCES(allocated)  (allocated)++
#define FREE_PIN_INSTANCE_RESOURCES(allocated)      (allocated)--

//=============================================================================
// Wave/topology bridge connection table
//=============================================================================
typedef enum
{
    CONNECTIONTYPE_TOPOLOGY_OUTPUT = 0,
    CONNECTIONTYPE_WAVE_OUTPUT     = 1
} CONNECTIONTYPE;

typedef struct _PHYSICALCONNECTIONTABLE
{
    ULONG            ulTopology;
    ULONG            ulWave;
    CONNECTIONTYPE   eType;
} PHYSICALCONNECTIONTABLE, *PPHYSICALCONNECTIONTABLE;

//=============================================================================
// PortCls FDO device extension. PortCls reserves the extension; the adapter
// common object pointer lives in the slot the sample used, so it can be
// released on stop/removal.
//=============================================================================
struct IAdapterCommon;
typedef struct _PortClassDeviceContext              // 32       64      Byte offsets for 32 and 64 bit architectures
{
    ULONG_PTR m_pulReserved1[2];                    // 0-7      0-15    First two pointers are reserved.
    PDEVICE_OBJECT m_DoNotUsePhysicalDeviceObject;  // 8-11     16-23   Reserved pointer to our Physical Device Object (PDO).
    PVOID m_pvReserved2;                            // 12-15    24-31   Reserved pointer to our Start Device function.
    PVOID m_pvReserved3;                            // 16-19    32-39   "Out Memory" according to DDK.
    IAdapterCommon* m_pCommon;                      // 20-23    40-47   Pointer to our adapter common object.
    PVOID m_pvUnused1;                              // 24-27    48-55   Unused space.
    PVOID m_pvUnused2;                              // 28-31    56-63   Unused space.

    // Anything after above line should not be used.
    // This actually goes on for (64*sizeof(ULONG_PTR)) but it is all opaque.
} PortClassDeviceContext;

//=============================================================================
// Function prototypes
//=============================================================================

// Wave filter automation (KSPROPSETID_Pin proposed formats).
NTSTATUS PropertyHandler_WaveFilter
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

#include "common.h"
#include "kshelper.h"
