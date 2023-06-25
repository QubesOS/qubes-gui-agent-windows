/*++

Module Name:

    public.h

Abstract:

    This module contains the common declarations shared by driver
    and user applications.

Environment:

    user and kernel

--*/

#pragma once
#include <windef.h>

//
// Define an Interface Guid so that apps can find the device and talk to it.
//

DEFINE_GUID (GUID_DEVINTERFACE_MemoryLock,
    0x0b193a5b,0xfdcd,0x43aa,0xa4,0xc7,0x14,0xfb,0x18,0x8e,0xee,0x63);
// {0b193a5b-fdcd-43aa-a4c7-14fb188eee63}

#define IOCTL_MEMORYLOCK_LOCK CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA)

typedef struct _MEMORYLOCK_LOCK_IN
{
    PVOID Address;
    UINT Size;
} MEMORYLOCK_LOCK_IN;

typedef struct _MEMORYLOCK_LOCK_OUT
{
    ULONG RequestId;
    size_t NumberOfPages;
} MEMORYLOCK_LOCK_OUT;

#define IOCTL_MEMORYLOCK_UNLOCK CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA)

typedef struct _MEMORYLOCK_UNLOCK_IN
{
    ULONG RequestId;
} MEMORYLOCK_UNLOCK_IN;

#define IOCTL_MEMORYLOCK_GET_PFNS CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA)

typedef struct _MEMORYLOCK_GET_PFNS_IN
{
    ULONG RequestId;
} MEMORYLOCK_GET_PFNS_IN;

#pragma warning(push)
#pragma warning(disable : 4200) // zero-length array

typedef struct _MEMORYLOCK_GET_PFNS_OUT
{
    size_t NumberOfPages;
    ULONG64 Pfn[];
} MEMORYLOCK_GET_PFNS_OUT;

#pragma warning(pop)
