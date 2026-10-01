/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    NewDelete.h

Abstract:

    Declaration of the pool-backed new and delete operators.

    Derived from Windows-driver-samples/audio/simpleaudiosample (MS-PL).
    Modified for MobileWebcamBridge: operators are noexcept so every new-expression
    checks for allocation failure, and delete never assumes a tag.
--*/
#pragma once

#ifdef _NEW_DELETE_OPERATORS_

// Allocates from the given pool with the given tag. Returns nullptr on failure.
PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag) noexcept;

// Allocates from the given pool with the driver's default tag. Returns nullptr on failure.
PVOID operator new(size_t iSize, POOL_FLAGS poolFlags) noexcept;

void __cdecl operator delete(PVOID pVoid, ULONG tag) noexcept;
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize) noexcept;
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid) noexcept;
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize) noexcept;
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid) noexcept;

#endif // _NEW_DELETE_OPERATORS_
