/*++

Module Name:

    driver.h

Abstract:

    This file contains the driver definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#include "device.h"
#include "queue.h"
#include "trace.h"

EXTERN_C_START

//
// WDFDRIVER Events
//

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MemoryLockEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP MemoryLockEvtDriverContextCleanup;

#define ML_POOL_TAG 'kclM'

#define DBGPRINT(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__)

EXTERN_C_END
