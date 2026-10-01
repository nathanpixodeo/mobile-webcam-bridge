/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    NewDelete.cpp

Abstract:

    Definition of the pool-backed new and delete operators.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: noexcept operators; ExFreePool (tag-agnostic) so
    objects allocated with a component tag can be deleted through any path.
--*/

#include "definitions.h"

#ifdef _NEW_DELETE_OPERATORS_

#pragma code_seg()

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag) noexcept
{
    return ExAllocatePool2(poolFlags, iSize, tag);
}

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags) noexcept
{
    return ExAllocatePool2(poolFlags, iSize, MWBMIC_ADAPTER_POOLTAG);
}

void __cdecl operator delete(PVOID pVoid, ULONG tag) noexcept
{
    UNREFERENCED_PARAMETER(tag);

    if (pVoid != nullptr)
    {
        ExFreePool(pVoid);
    }
}

void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize) noexcept
{
    UNREFERENCED_PARAMETER(cbSize);

    if (pVoid != nullptr)
    {
        ExFreePool(pVoid);
    }
}

void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid) noexcept
{
    if (pVoid != nullptr)
    {
        ExFreePool(pVoid);
    }
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize) noexcept
{
    UNREFERENCED_PARAMETER(cbSize);

    if (pVoid != nullptr)
    {
        ExFreePool(pVoid);
    }
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid) noexcept
{
    if (pVoid != nullptr)
    {
        ExFreePool(pVoid);
    }
}

#endif // _NEW_DELETE_OPERATORS_
