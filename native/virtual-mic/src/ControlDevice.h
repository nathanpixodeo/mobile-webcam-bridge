// MobileWebcamBridge virtual microphone — feed control device (\Device\MobileWebcamBridgeMic).
//
// A non-PnP control device object next to the PortCls FDO. User mode opens it as
// \\.\MobileWebcamBridgeMic and pushes PCM with IOCTL_MWBMIC_WRITE (protocol/MIC_FEED.md).
// The driver routes every IRP: those addressed to this device are handled here, all others go
// to PortCls.
#pragma once

class MicFeedBuffer;

namespace ControlDevice
{
    // Creates the device and its symbolic link, bound to `feed`. Called from StartDevice.
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS Create(_In_ PDRIVER_OBJECT driverObject, _In_ MicFeedBuffer* feed);

    // Unbinds the feed (waiting for in-flight requests), removes the symbolic link and deletes
    // the device. Handles still open afterwards fail every request with STATUS_DEVICE_REMOVED.
    // Called on stop, surprise removal and removal so the adapter can go and the driver unload.
    _IRQL_requires_(PASSIVE_LEVEL)
    void Delete();

    // True when `deviceObject` is a feed control device (current or already deleted).
    _IRQL_requires_max_(DISPATCH_LEVEL)
    bool Owns(_In_ PDEVICE_OBJECT deviceObject);

    // Completes the IRP. Only CREATE, CLEANUP, CLOSE and DEVICE_CONTROL are supported.
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS Dispatch(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp);
}
