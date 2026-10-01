# Virtual microphone feed interface — ABI version 1

`mwbmic.sys` exposes one capture endpoint ("Mobile Webcam Microphone"). User mode feeds PCM into
it through a **control device object** (CDO), separate from the PortCls audio filters.

C definition (kernel and user mode): `native/common/include/mwb/MwbMicIoctl.h`.

## 1. Device

- Path: `\\.\MobileWebcamBridgeMic` (kernel symlink `\DosDevices\Global\MobileWebcamBridgeMic`).
- Security: `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)` — SYSTEM, Administrators and
  interactive users.
- **Exclusive**: one open handle at a time. A second `CreateFileW` fails with
  `ERROR_SHARING_VIOLATION`. When the handle closes (`IRP_MJ_CLEANUP`, including process exit),
  the driver resets the ring buffer, so a crashed feeder never leaves stale audio behind.

## 2. Audio format

Fixed in v1: PCM signed 16-bit little-endian, 48 000 Hz, 1 channel (block align 2 bytes).

## 3. IOCTLs

All IOCTLs use `METHOD_BUFFERED` and device type `FILE_DEVICE_UNKNOWN` (`0x22`).

| Name                       | Function | Access            | Input              | Output            |
|----------------------------|---------:|-------------------|--------------------|-------------------|
| `IOCTL_MWBMIC_GET_VERSION` | `0x900`  | `FILE_READ_ACCESS`  | —                | `MWBMIC_VERSION`  |
| `IOCTL_MWBMIC_WRITE`       | `0x901`  | `FILE_WRITE_ACCESS` | PCM bytes        | `MWBMIC_STATUS`   |
| `IOCTL_MWBMIC_GET_STATUS`  | `0x902`  | `FILE_READ_ACCESS`  | —                | `MWBMIC_STATUS`   |
| `IOCTL_MWBMIC_RESET`       | `0x903`  | `FILE_WRITE_ACCESS` | —                | —                 |

### `IOCTL_MWBMIC_WRITE`

- Input length must be even and ≤ `MWBMIC_MAX_WRITE_BYTES` (16 384); otherwise
  `STATUS_INVALID_PARAMETER`.
- Output buffer must hold an `MWBMIC_STATUS`; the driver consumes the input before writing the
  output (they share the system buffer).
- While no capture stream is running, written audio is discarded (the call still succeeds and
  the status reports `STREAM_ACTIVE = 0`).
- When the ring is full, the oldest audio is dropped and `OverrunCount` increments.

### Structures

```c
typedef struct _MWBMIC_VERSION {      // 20 bytes
    UINT32 StructSize;                // sizeof(MWBMIC_VERSION)
    UINT32 AbiVersion;                // MWBMIC_ABI_VERSION (1)
    UINT32 SampleRate;                // 48000
    UINT16 Channels;                  // 1
    UINT16 BitsPerSample;             // 16
    UINT32 RingCapacityBytes;         // 65536
} MWBMIC_VERSION;

typedef struct _MWBMIC_STATUS {       // 40 bytes
    UINT32 StructSize;                // sizeof(MWBMIC_STATUS)
    UINT32 Flags;                     // MWBMIC_STATUS_FLAG_STREAM_ACTIVE = 0x1
    UINT32 BufferedBytes;             // bytes waiting in the ring
    UINT32 CapacityBytes;             // ring capacity
    UINT32 UnderrunCount;             // reads that found too little data (zero-filled)
    UINT32 OverrunCount;              // writes that dropped old data
    UINT64 TotalBytesWritten;         // accepted into the ring since driver load
    UINT64 TotalBytesRead;            // consumed by the capture stream since driver load
} MWBMIC_STATUS;
```

## 4. Clocking and drift

The capture stream consumes audio at the driver clock (QPC-based timer). The phone produces
audio at its own clock. The driver stays dumb (zero-fill on underrun, drop-oldest on overrun);
drift compensation happens in user mode: Node resamples by a few hundred ppm so that
`BufferedBytes` stays near its target (40 ms = 3 840 bytes).
