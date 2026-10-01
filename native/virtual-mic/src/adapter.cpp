/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    adapter.cpp

Abstract:

    Driver entry, device start and IRP routing for the MobileWebcamBridge virtual
    microphone. No hardware resources are used.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: WDF and registry settings removed; one capture
    endpoint; every IRP is routed either to the feed control device or to
    PortCls; the control device lives from StartDevice to stop/removal.
--*/

//
// All the GUIDs for all the miniports end up in this object.
//
#define PUT_GUIDS_HERE
#include <initguid.h>

#include "definitions.h"
#include "endpoints.h"
#include "minipairs.h"
#include "ControlDevice.h"
#include "MicFeedBuffer.h"

typedef void (*fnPcDriverUnload) (PDRIVER_OBJECT);
fnPcDriverUnload gPCDriverUnloadRoutine = NULL;

extern "C" DRIVER_UNLOAD DriverUnload;
DRIVER_ADD_DEVICE AddDevice;

NTSTATUS
StartDevice
(
    _In_  PDEVICE_OBJECT,
    _In_  PIRP,
    _In_  PRESOURCELIST
);

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH PnpHandler;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_Dispatch_type_(IRP_MJ_PNP)
_Dispatch_type_(IRP_MJ_POWER)
_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH RouteIrp;

//=============================================================================
#pragma code_seg("PAGE")
extern "C"
void DriverUnload
(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    PAGED_CODE();

    DPF(D_TERSE, ("[DriverUnload]"));

    if (DriverObject != NULL && gPCDriverUnloadRoutine != NULL)
    {
        gPCDriverUnloadRoutine(DriverObject);
    }
}

//=============================================================================
#pragma code_seg("INIT")
extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS
DriverEntry
(
    _In_  PDRIVER_OBJECT          DriverObject,
    _In_  PUNICODE_STRING         RegistryPathName
)
{
    DPF(D_TERSE, ("[DriverEntry]"));

    NTSTATUS ntStatus = PcInitializeAdapterDriver(DriverObject, RegistryPathName, (PDRIVER_ADD_DEVICE)AddDevice);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_TERSE, ("PcInitializeAdapterDriver failed, 0x%x", ntStatus));
        return ntStatus;
    }

    // PortCls owns the dispatch table now. Route every major function through RouteIrp:
    // IRPs for the feed control device are handled by ControlDevice, all others go to PortCls
    // (PnP through PnpHandler for cleanup first). Overriding all majors, not just the four the
    // control device supports, keeps any other IRP sent to the control device away from PortCls.
    for (ULONG major = 0; major <= IRP_MJ_MAXIMUM_FUNCTION; ++major)
    {
        DriverObject->MajorFunction[major] = RouteIrp;
    }

    // Hook the PortCls unload routine.
    gPCDriverUnloadRoutine = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnload;

    return STATUS_SUCCESS;
} // DriverEntry

#pragma code_seg()
// disable prefast warning 28152 because DO_DEVICE_INITIALIZING is cleared in PcAddAdapterDevice
#pragma warning(disable:28152)
#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS AddDevice
(
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PDEVICE_OBJECT    PhysicalDeviceObject
)
{
    PAGED_CODE();

    DPF(D_TERSE, ("[AddDevice]"));

    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              PCPFNSTARTDEVICE(StartDevice),
                              g_MaxMiniports,
                              0);
} // AddDevice

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
StartDevice
(
    _In_  PDEVICE_OBJECT          DeviceObject,
    _In_  PIRP                    Irp,
    _In_  PRESOURCELIST           ResourceList
)
/*++

Routine Description:

  Called by PortCls when the device starts: creates the adapter object, the
  microphone endpoint and the feed control device.

--*/
{
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(DeviceObject);
    ASSERT(Irp);

    PADAPTERCOMMON              pAdapterCommon  = NULL;
    PUNKNOWN                    pUnknownCommon  = NULL;
    PortClassDeviceContext*     pExtension      = static_cast<PortClassDeviceContext*>(DeviceObject->DeviceExtension);

    DPF_ENTER(("[StartDevice]"));

    NTSTATUS ntStatus = NewAdapterCommon(&pUnknownCommon, IID_IAdapterCommon, NULL, POOL_FLAG_NON_PAGED);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = pUnknownCommon->QueryInterface(IID_IAdapterCommon, (PVOID *)&pAdapterCommon);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = pAdapterCommon->Init(DeviceObject);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterAdapterPowerManagement(PUNKNOWN(pAdapterCommon), DeviceObject);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = pAdapterCommon->InstallEndpointFilters(Irp, &MicMiniports);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = ControlDevice::Create(DeviceObject->DriverObject, pAdapterCommon->GetMicFeedBuffer());
    }

    // Stash the adapter in the device extension even on failure: PnP follows a failed start
    // with a removal, and PnpHandler releases it there.
    if (pAdapterCommon)
    {
        ASSERT(pExtension != NULL);
        pExtension->m_pCommon = pAdapterCommon;
    }

    SAFE_RELEASE(pUnknownCommon);

    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_TERSE, ("StartDevice failed, 0x%x", ntStatus));
    }

    return ntStatus;
} // StartDevice

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PnpHandler
(
    _In_ DEVICE_OBJECT *_DeviceObject,
    _Inout_ IRP *_Irp
)
/*++

Routine Description:

  Releases the control device and the adapter on stop and removal, then hands
  the IRP to PortCls. PnP IRPs always arrive at PASSIVE_LEVEL.

--*/
{
#pragma warning(suppress: 28118)
    PAGED_CODE();

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    IO_STACK_LOCATION* stack = IoGetCurrentIrpStackLocation(_Irp);

    switch (stack->MinorFunction)
    {
    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_STOP_DEVICE:
    {
        // The control device references the adapter's feed buffer: unbind it first.
        ControlDevice::Delete();

        PortClassDeviceContext* ext = static_cast<PortClassDeviceContext*>(_DeviceObject->DeviceExtension);
        if (ext->m_pCommon != NULL)
        {
            ext->m_pCommon->Release();
            ext->m_pCommon = NULL;
        }
        break;
    }

    default:
        break;
    }

    return PcDispatchIrp(_DeviceObject, _Irp);
}

//=============================================================================
#pragma code_seg()
NTSTATUS
RouteIrp
(
    _In_ DEVICE_OBJECT *DeviceObject,
    _Inout_ IRP *Irp
)
{
    if (ControlDevice::Owns(DeviceObject))
    {
        return ControlDevice::Dispatch(DeviceObject, Irp);
    }

    if (IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_PNP)
    {
        return PnpHandler(DeviceObject, Irp);
    }

    return PcDispatchIrp(DeviceObject, Irp);
}

#pragma code_seg()
