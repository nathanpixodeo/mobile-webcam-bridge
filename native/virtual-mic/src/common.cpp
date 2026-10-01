/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    common.cpp

Abstract:

    Implementation of the adapter common object.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: WDF miniport, mixer hardware, SaveData, ETW,
    subdevice cache and device interface templates removed; the adapter owns the
    microphone feed buffer.
--*/

#include "definitions.h"
#include "endpoints.h"
#include "MicFeedBuffer.h"

//=============================================================================
// CAdapterCommon
//=============================================================================
class CAdapterCommon :
    public IAdapterCommon,
    public IAdapterPowerManagement,
    public CUnknown
{
private:
    PDEVICE_OBJECT          m_pDeviceObject;
    DEVICE_POWER_STATE      m_PowerState;
    MicFeedBuffer           m_MicFeed;

    static LONG             m_AdapterInstances;     // the control device name is global: one adapter only

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CAdapterCommon);
    ~CAdapterCommon();

    IMP_IAdapterPowerManagement;

    STDMETHODIMP_(NTSTATUS)         Init(_In_ PDEVICE_OBJECT DeviceObject);
    STDMETHODIMP_(PDEVICE_OBJECT)   GetDeviceObject(void);
    STDMETHODIMP_(MicFeedBuffer*)   GetMicFeedBuffer(void);
    STDMETHODIMP_(NTSTATUS)         InstallEndpointFilters(_In_opt_ PIRP Irp, _In_ PENDPOINT_MINIPAIR MiniportPair);

    friend NTSTATUS NewAdapterCommon
    (
        _Out_       PUNKNOWN *              Unknown,
        _In_        REFCLSID,
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        POOL_FLAGS              PoolFlags
    );

private:
    NTSTATUS InstallSubdevice
    (
        _In_opt_    PIRP                Irp,
        _In_        PWSTR               Name,
        _In_        REFGUID             PortClassId,
        _In_        PFNCREATEMINIPORT   MiniportCreate,
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _Out_       PUNKNOWN          * OutPortUnknown
    );

    NTSTATUS ConnectTopologies
    (
        _In_ PUNKNOWN                   UnknownTopology,
        _In_ PUNKNOWN                   UnknownWave,
        _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
        _In_ ULONG                      PhysicalConnectionCount
    );
};

LONG CAdapterCommon::m_AdapterInstances = 0;

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
NewAdapterCommon
(
    _Out_       PUNKNOWN *              Unknown,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        POOL_FLAGS              PoolFlags
)
{
    PAGED_CODE();

    ASSERT(Unknown);

    if (InterlockedCompareExchange(&CAdapterCommon::m_AdapterInstances, 1, 0) != 0)
    {
        DPF(D_TERSE, ("NewAdapterCommon failed, only one instance is allowed"));
        return STATUS_DEVICE_BUSY;
    }

    // Non-paged: the feed buffer's spinlock and counters are used at DISPATCH_LEVEL.
    CAdapterCommon *p = new(PoolFlags, MWBMIC_ADAPTER_POOLTAG) CAdapterCommon(UnknownOuter);
    if (p == NULL)
    {
        InterlockedExchange(&CAdapterCommon::m_AdapterInstances, 0);
        DPF(D_TERSE, ("NewAdapterCommon failed, insufficient resources"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Unknown = PUNKNOWN((PADAPTERCOMMON)(p));
    (*Unknown)->AddRef();

    return STATUS_SUCCESS;
} // NewAdapterCommon

//=============================================================================
#pragma code_seg("PAGE")
CAdapterCommon::~CAdapterCommon
(
    void
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::~CAdapterCommon]"));

    InterlockedExchange(&CAdapterCommon::m_AdapterInstances, 0);
} // ~CAdapterCommon

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::Init
(
    _In_  PDEVICE_OBJECT          DeviceObject
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::Init]"));

    ASSERT(DeviceObject);

    m_pDeviceObject = DeviceObject;
    m_PowerState    = PowerDeviceD0;

    return m_MicFeed.Initialize();
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT)
CAdapterCommon::GetDeviceObject
(
    void
)
{
    PAGED_CODE();

    return m_pDeviceObject;
} // GetDeviceObject

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(MicFeedBuffer*)
CAdapterCommon::GetMicFeedBuffer
(
    void
)
{
    return &m_MicFeed;
} // GetMicFeedBuffer

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP
CAdapterCommon::NonDelegatingQueryInterface
(
    _In_ REFIID                      Interface,
    _COM_Outptr_ PVOID *             Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PADAPTERCOMMON(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterCommon))
    {
        *Object = PVOID(PADAPTERCOMMON(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement))
    {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
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
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::PowerChangeState
(
    _In_  POWER_STATE             NewState
)
/*++

Routine Description:

  PortCls pauses active streams before a sleep transition and resumes them
  afterwards. The virtual device has no hardware state to save; it only tracks
  the state.

--*/
{
    DPF_ENTER(("[CAdapterCommon::PowerChangeState]"));

    switch (NewState.DeviceState)
    {
        case PowerDeviceD0:
        case PowerDeviceD1:
        case PowerDeviceD2:
        case PowerDeviceD3:
            m_PowerState = NewState.DeviceState;
            break;

        default:
            DPF(D_VERBOSE, ("Unknown Device Power State"));
            break;
    }
} // PowerChangeState

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryDeviceCapabilities
(
    _Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES)) PDEVICE_CAPABILITIES    PowerDeviceCaps
)
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);

    DPF_ENTER(("[CAdapterCommon::QueryDeviceCapabilities]"));

    return STATUS_SUCCESS;
} // QueryDeviceCapabilities

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryPowerChangeState
(
    _In_  POWER_STATE             NewStateQuery
)
{
    UNREFERENCED_PARAMETER(NewStateQuery);

    DPF_ENTER(("[CAdapterCommon::QueryPowerChangeState]"));

    return STATUS_SUCCESS;
} // QueryPowerChangeState

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::InstallSubdevice
(
    _In_opt_    PIRP                Irp,
    _In_        PWSTR               Name,
    _In_        REFGUID             PortClassId,
    _In_        PFNCREATEMINIPORT   MiniportCreate,
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _Out_       PUNKNOWN          * OutPortUnknown
)
/*++

Routine Description:

  Creates a port and its miniport, binds them and registers the pair as a KS
  subdevice named Name. PcRegisterSubdevice also registers and enables the
  device interfaces for the filter's categories.

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[InstallSubDevice %S]", Name));

    ASSERT(Name != NULL);
    ASSERT(MiniportCreate != NULL);
    ASSERT(m_pDeviceObject != NULL);

    PPORT       port     = NULL;
    PUNKNOWN    miniport = NULL;

    *OutPortUnknown = NULL;

    NTSTATUS ntStatus = PcNewPort(&port, PortClassId);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = MiniportCreate(&miniport, PortClassId, NULL, POOL_FLAG_NON_PAGED, MiniportPair);
    }

    if (NT_SUCCESS(ntStatus))
    {
#pragma warning(push)
        // IPort::Init's annotation requires a resource list; a root-enumerated virtual device
        // has none, which PortCls accepts.
#pragma warning(disable:6387)
        ntStatus = port->Init(m_pDeviceObject, Irp, miniport, PUNKNOWN(PADAPTERCOMMON(this)), NULL);
#pragma warning(pop)
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterSubdevice(m_pDeviceObject, Name, port);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = port->QueryInterface(IID_IUnknown, (PVOID *)OutPortUnknown);
    }

    SAFE_RELEASE(miniport);
    SAFE_RELEASE(port);

    return ntStatus;
} // InstallSubdevice

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::ConnectTopologies
(
    _In_ PUNKNOWN                   UnknownTopology,
    _In_ PUNKNOWN                   UnknownWave,
    _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
    _In_ ULONG                      PhysicalConnectionCount
)
/*++

Routine Description:

  Registers the physical connections between the wave and topology bridge pins.

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::ConnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    for (ULONG i = 0; i < PhysicalConnectionCount && NT_SUCCESS(ntStatus); i++)
    {
        switch (PhysicalConnections[i].eType)
        {
            case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                ntStatus = PcRegisterPhysicalConnection(m_pDeviceObject,
                                                        UnknownTopology,
                                                        PhysicalConnections[i].ulTopology,
                                                        UnknownWave,
                                                        PhysicalConnections[i].ulWave);
                break;

            case CONNECTIONTYPE_WAVE_OUTPUT:
                ntStatus = PcRegisterPhysicalConnection(m_pDeviceObject,
                                                        UnknownWave,
                                                        PhysicalConnections[i].ulWave,
                                                        UnknownTopology,
                                                        PhysicalConnections[i].ulTopology);
                break;

            default:
                ntStatus = STATUS_INVALID_PARAMETER;
                break;
        }

        if (!NT_SUCCESS(ntStatus))
        {
            DPF(D_TERSE, ("ConnectTopologies: PcRegisterPhysicalConnection failed, 0x%x", ntStatus));
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::InstallEndpointFilters
(
    _In_opt_    PIRP                Irp,
    _In_        PENDPOINT_MINIPAIR  MiniportPair
)
/*++

Routine Description:

  Installs the topology and wave subdevices of one endpoint and connects them.
  PortCls keeps its own references to registered subdevices and unregisters
  them when the device is removed, so the adapter keeps none.

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::InstallEndpointFilters]"));

    PUNKNOWN unknownTopology = NULL;
    PUNKNOWN unknownWave     = NULL;

    NTSTATUS ntStatus = InstallSubdevice(Irp,
                                         MiniportPair->TopoName,
                                         CLSID_PortTopology,
                                         MiniportPair->TopoCreateCallback,
                                         MiniportPair,
                                         &unknownTopology);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = InstallSubdevice(Irp,
                                    MiniportPair->WaveName,
                                    CLSID_PortWaveRT,
                                    MiniportPair->WaveCreateCallback,
                                    MiniportPair,
                                    &unknownWave);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = ConnectTopologies(unknownTopology,
                                     unknownWave,
                                     MiniportPair->PhysicalConnections,
                                     MiniportPair->PhysicalConnectionCount);
    }

    SAFE_RELEASE(unknownTopology);
    SAFE_RELEASE(unknownWave);

    return ntStatus;
}

#pragma code_seg()
