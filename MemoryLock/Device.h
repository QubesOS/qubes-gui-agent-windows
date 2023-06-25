/*++

Module Name:

    device.h

Abstract:

    This file contains the device definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "MemoryLockPublic.h"

EXTERN_C_START

// Bookkeeping information about a single memory lock request.
typedef struct _LOCK_ENTRY
{
    PEPROCESS Process;
    ULONG RequestId;
    MDL* Mdl;
    LIST_ENTRY List;
} LOCK_ENTRY;

// Context for a single file object (user handle).
typedef struct _FILE_CONTEXT
{
    LIST_ENTRY Requests;
} FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE(FILE_CONTEXT);

//
// The device context performs the same job as
// a WDM device extension in the driver frameworks
//
typedef struct _DEVICE_CONTEXT
{
    ULONG LastRequestId;
} DEVICE_CONTEXT;

//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

//
// Function to initialize the device and its callbacks
//
NTSTATUS
MemoryLockCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

void DumpRequests(
    _In_ PLIST_ENTRY ListHead
);

// File callbacks
EVT_WDF_DEVICE_FILE_CREATE MemoryLockEvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE MemoryLockEvtFileClose;

EXTERN_C_END
