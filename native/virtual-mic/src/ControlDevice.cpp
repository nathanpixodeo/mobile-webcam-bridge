// MobileWebcamBridge virtual microphone — feed control device (\Device\MobileWebcamBridgeMic).

#include "definitions.h"
#include <wdmsec.h>
#include "ControlDevice.h"
#include "MicFeedBuffer.h"

namespace
{
    // Marks our device extension. No kernel pointer can take this value, so reading the first
    // eight bytes of a foreign extension can never produce a false positive.
    constexpr ULONG64 kExtensionSignature = 0x7863694D42504921ull;

    // {F2A5FB09-1C39-45DE-89D3-87EE371439EE} — device class passed to IoCreateDeviceSecure, so
    // an administrator can override the default security through the class registry key.
    const GUID kControlDeviceClassGuid =
        { 0xf2a5fb09, 0x1c39, 0x45de, { 0x89, 0xd3, 0x87, 0xee, 0x37, 0x14, 0x39, 0xee } };

    struct ControlDeviceExtension
    {
        ULONG64         Signature;
        EX_RUNDOWN_REF  Rundown;      // guards Feed: requests hold it only while they run
        MicFeedBuffer*  Feed;
        volatile LONG   HandleOpen;   // 1 while the (single) feeder handle is open
    };

    // Only touched from StartDevice and the PnP stop/remove paths, which PnP serialises.
    PDEVICE_OBJECT g_controlDevice = nullptr;

    ControlDeviceExtension* ExtensionOf(_In_ PDEVICE_OBJECT deviceObject)
    {
        return static_cast<ControlDeviceExtension*>(deviceObject->DeviceExtension);
    }

    // Scoped rundown reference: while acquired, the feed buffer cannot be unbound.
    class FeedAccess final
    {
    public:
        explicit FeedAccess(_In_ ControlDeviceExtension* extension)
            : m_extension(extension),
              m_acquired(ExAcquireRundownProtection(&extension->Rundown) != FALSE)
        {
        }

        ~FeedAccess()
        {
            if (m_acquired)
            {
                ExReleaseRundownProtection(&m_extension->Rundown);
            }
        }

        FeedAccess(const FeedAccess&) = delete;
        FeedAccess& operator=(const FeedAccess&) = delete;

        bool Acquired() const { return m_acquired; }
        MicFeedBuffer& Feed() const { return *m_extension->Feed; }

    private:
        ControlDeviceExtension* m_extension;
        bool                    m_acquired;
    };

    NTSTATUS OnCreate(_In_ ControlDeviceExtension* extension, _In_ PIO_STACK_LOCATION stack)
    {
        // The device has no namespace below it (\\.\MobileWebcamBridgeMic\anything is not a thing).
        if (stack->FileObject != nullptr && stack->FileObject->FileName.Length != 0)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        FeedAccess access(extension);
        if (!access.Acquired())
        {
            return STATUS_DELETE_PENDING;
        }

        // One feeder at a time: two writers would interleave PCM.
        if (InterlockedCompareExchange(&extension->HandleOpen, 1, 0) != 0)
        {
            return STATUS_SHARING_VIOLATION;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS OnCleanup(_In_ ControlDeviceExtension* extension)
    {
        // The feeder went away (normally or by crashing): never leave its audio behind.
        {
            FeedAccess access(extension);
            if (access.Acquired())
            {
                access.Feed().Reset();
            }
        }

        InterlockedExchange(&extension->HandleOpen, 0);
        return STATUS_SUCCESS;
    }

    NTSTATUS HandleGetVersion(_Out_writes_bytes_opt_(outputLength) PVOID buffer,
                              _In_ ULONG outputLength,
                              _Out_ ULONG_PTR* information)
    {
        *information = 0;
        if (buffer == nullptr || outputLength < sizeof(MWBMIC_VERSION))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        MWBMIC_VERSION version = {};
        version.StructSize        = sizeof(MWBMIC_VERSION);
        version.AbiVersion        = MWBMIC_ABI_VERSION;
        version.SampleRate        = MWBMIC_SAMPLE_RATE;
        version.Channels          = static_cast<UINT16>(MWBMIC_CHANNELS);
        version.BitsPerSample     = static_cast<UINT16>(MWBMIC_BITS_PER_SAMPLE);
        version.RingCapacityBytes = MWBMIC_RING_CAPACITY_BYTES;

        RtlCopyMemory(buffer, &version, sizeof(version));
        *information = sizeof(version);
        return STATUS_SUCCESS;
    }

    NTSTATUS HandleWrite(_In_ MicFeedBuffer& feed,
                         _Inout_updates_bytes_opt_(bufferLength) PVOID buffer,
                         _In_ ULONG bufferLength,
                         _In_ ULONG inputLength,
                         _In_ ULONG outputLength,
                         _Out_ ULONG_PTR* information)
    {
        *information = 0;
        if (buffer == nullptr || outputLength < sizeof(MWBMIC_STATUS))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (inputLength > MWBMIC_MAX_WRITE_BYTES || inputLength > bufferLength ||
            (inputLength % MWBMIC_BLOCK_ALIGN) != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        // METHOD_BUFFERED: input and output share the system buffer. Write() consumes the input
        // completely before the status is copied over it.
        MWBMIC_STATUS status;
        feed.Write(static_cast<const UCHAR*>(buffer), inputLength, &status);

        RtlCopyMemory(buffer, &status, sizeof(status));
        *information = sizeof(status);
        return STATUS_SUCCESS;
    }

    NTSTATUS HandleGetStatus(_In_ MicFeedBuffer& feed,
                             _Out_writes_bytes_opt_(outputLength) PVOID buffer,
                             _In_ ULONG outputLength,
                             _Out_ ULONG_PTR* information)
    {
        *information = 0;
        if (buffer == nullptr || outputLength < sizeof(MWBMIC_STATUS))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        MWBMIC_STATUS status;
        feed.QueryStatus(&status);

        RtlCopyMemory(buffer, &status, sizeof(status));
        *information = sizeof(status);
        return STATUS_SUCCESS;
    }

    NTSTATUS OnDeviceControl(_In_ ControlDeviceExtension* extension,
                             _In_ PIRP irp,
                             _In_ PIO_STACK_LOCATION stack,
                             _Out_ ULONG_PTR* information)
    {
        *information = 0;

        FeedAccess access(extension);
        if (!access.Acquired())
        {
            return STATUS_DEVICE_REMOVED;
        }

        const ULONG inputLength  = stack->Parameters.DeviceIoControl.InputBufferLength;
        const ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
        const ULONG bufferLength = inputLength > outputLength ? inputLength : outputLength;
        PVOID buffer = irp->AssociatedIrp.SystemBuffer;

        switch (stack->Parameters.DeviceIoControl.IoControlCode)
        {
        case IOCTL_MWBMIC_GET_VERSION:
            return HandleGetVersion(buffer, outputLength, information);

        case IOCTL_MWBMIC_WRITE:
            return HandleWrite(access.Feed(), buffer, bufferLength, inputLength, outputLength, information);

        case IOCTL_MWBMIC_GET_STATUS:
            return HandleGetStatus(access.Feed(), buffer, outputLength, information);

        case IOCTL_MWBMIC_RESET:
            access.Feed().Reset();
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
} // namespace

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS ControlDevice::Create(PDRIVER_OBJECT driverObject, MicFeedBuffer* feed)
{
    PAGED_CODE();

    NT_ASSERTMSG("feed control device created twice", g_controlDevice == nullptr);
    if (g_controlDevice != nullptr)
    {
        return STATUS_OBJECT_NAME_COLLISION;
    }

    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(MWBMIC_DEVICE_NAME_W);
    UNICODE_STRING symbolicLink = RTL_CONSTANT_STRING(MWBMIC_SYMLINK_NAME_W);
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(MWBMIC_DEVICE_SDDL_W);

    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(driverObject,
                                           sizeof(ControlDeviceExtension),
                                           &deviceName,
                                           FILE_DEVICE_UNKNOWN,
                                           FILE_DEVICE_SECURE_OPEN,
                                           FALSE,                // exclusivity is enforced in OnCreate
                                           &sddl,
                                           &kControlDeviceClassGuid,
                                           &device);
    if (!NT_SUCCESS(status))
    {
        DPF(D_TERSE, ("IoCreateDeviceSecure failed, 0x%x", status));
        return status;
    }

    ControlDeviceExtension* extension = ExtensionOf(device);
    extension->Signature = kExtensionSignature;
    ExInitializeRundownProtection(&extension->Rundown);
    extension->Feed = feed;
    extension->HandleOpen = 0;

    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        DPF(D_TERSE, ("IoCreateSymbolicLink failed, 0x%x", status));
        IoDeleteDevice(device);
        return status;
    }

    device->Flags &= ~DO_DEVICE_INITIALIZING;
    g_controlDevice = device;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
void ControlDevice::Delete()
{
    PAGED_CODE();

    PDEVICE_OBJECT device = g_controlDevice;
    if (device == nullptr)
    {
        return;
    }
    g_controlDevice = nullptr;

    UNICODE_STRING symbolicLink = RTL_CONSTANT_STRING(MWBMIC_SYMLINK_NAME_W);
    (void)IoDeleteSymbolicLink(&symbolicLink);

    // Wait for in-flight requests; afterwards every request on a still-open handle fails
    // before it can touch the feed buffer, which goes away with the adapter.
    ControlDeviceExtension* extension = ExtensionOf(device);
    ExWaitForRundownProtectionRelease(&extension->Rundown);
    extension->Feed = nullptr;

    // The object itself lives on until the last open handle is closed.
    IoDeleteDevice(device);
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
bool ControlDevice::Owns(PDEVICE_OBJECT deviceObject)
{
    if (deviceObject == nullptr ||
        deviceObject->DeviceType != FILE_DEVICE_UNKNOWN ||
        deviceObject->DeviceExtension == nullptr)
    {
        return false;
    }

    return ExtensionOf(deviceObject)->Signature == kExtensionSignature;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
NTSTATUS ControlDevice::Dispatch(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    ControlDeviceExtension* extension = ExtensionOf(deviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);

    ULONG_PTR information = 0;
    NTSTATUS status;

    switch (stack->MajorFunction)
    {
    case IRP_MJ_CREATE:
        status = OnCreate(extension, stack);
        break;

    case IRP_MJ_CLEANUP:
        status = OnCleanup(extension);
        break;

    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;

    case IRP_MJ_DEVICE_CONTROL:
        status = OnDeviceControl(extension, irp, stack, &information);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
