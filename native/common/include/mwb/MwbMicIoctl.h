/*
 * Virtual microphone feed ABI v1 — the contract between `bridge-native mic feed` and
 * mwbmic.sys. Specification: protocol/MIC_FEED.md.
 *
 * Plain C so it compiles in the kernel driver (/kernel defines _KERNEL_MODE) and in user mode.
 */
#pragma once

#ifdef _KERNEL_MODE
#include <wdm.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define MWBMIC_ABI_VERSION 1u

#define MWBMIC_DEVICE_NAME_W  L"\\Device\\MobileWebcamBridgeMic"
#define MWBMIC_SYMLINK_NAME_W L"\\DosDevices\\Global\\MobileWebcamBridgeMic"
#define MWBMIC_USER_PATH_W    L"\\\\.\\MobileWebcamBridgeMic"
#define MWBMIC_HARDWARE_ID_W  L"ROOT\\MobileWebcamBridgeMic"
#define MWBMIC_DEVICE_SDDL_W  L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)"

#define MWBMIC_SAMPLE_RATE         48000u
#define MWBMIC_CHANNELS            1u
#define MWBMIC_BITS_PER_SAMPLE     16u
#define MWBMIC_BLOCK_ALIGN         2u
#define MWBMIC_RING_CAPACITY_BYTES 65536u
#define MWBMIC_MAX_WRITE_BYTES     16384u

#define MWBMIC_STATUS_FLAG_STREAM_ACTIVE 0x1u

#define IOCTL_MWBMIC_GET_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MWBMIC_WRITE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MWBMIC_GET_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MWBMIC_RESET       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _MWBMIC_VERSION {
    UINT32 StructSize;        /* sizeof(MWBMIC_VERSION) */
    UINT32 AbiVersion;        /* MWBMIC_ABI_VERSION */
    UINT32 SampleRate;        /* MWBMIC_SAMPLE_RATE */
    UINT16 Channels;          /* MWBMIC_CHANNELS */
    UINT16 BitsPerSample;     /* MWBMIC_BITS_PER_SAMPLE */
    UINT32 RingCapacityBytes; /* MWBMIC_RING_CAPACITY_BYTES */
} MWBMIC_VERSION;

typedef struct _MWBMIC_STATUS {
    UINT32 StructSize;        /* sizeof(MWBMIC_STATUS) */
    UINT32 Flags;             /* MWBMIC_STATUS_FLAG_* */
    UINT32 BufferedBytes;     /* bytes waiting in the ring */
    UINT32 CapacityBytes;     /* ring capacity */
    UINT32 UnderrunCount;     /* reads that found too little data (zero-filled) */
    UINT32 OverrunCount;      /* writes that dropped old data */
    UINT64 TotalBytesWritten; /* accepted into the ring since driver load */
    UINT64 TotalBytesRead;    /* consumed by the capture stream since driver load */
} MWBMIC_STATUS;

C_ASSERT(sizeof(MWBMIC_VERSION) == 20);
C_ASSERT(sizeof(MWBMIC_STATUS) == 40);
