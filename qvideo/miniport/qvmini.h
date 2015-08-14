#pragma warning(disable: 4201)
#include <dderror.h>
#include <devioctl.h>
#include <miniport.h>
#include <ntddvdeo.h>
#include <video.h>

// miniport headers don't include list macros for some ungodly reason...
#include "list.h"
#include "memory.h"

#define QFN "[QVMINI] " __FUNCTION__ ": "

// device extension, per-adapter data
typedef struct _QVMINI_DX
{
    PSPIN_LOCK BufferListLock;
    LIST_ENTRY BufferList;
} QVMINI_DX, *PQVMINI_DX;

VP_STATUS __checkReturn HwVidFindAdapter(
    __in void *HwDeviceExtension,
    __in void *HwContext,
    __in WCHAR *ArgumentString,
    __inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) VIDEO_PORT_CONFIG_INFO *ConfigInfo,
    __out UCHAR *Again
    );

BOOLEAN __checkReturn HwVidInitialize(
    __in void *HwDeviceExtension
    );

BOOLEAN __checkReturn HwVidStartIO(
    __in void *HwDeviceExtension,
    __in_bcount(sizeof(VIDEO_REQUEST_PACKET)) VIDEO_REQUEST_PACKET *RequestPacket
    );

ULONG __checkReturn DriverEntry(
    __in void *Context1,
    __in void *Context2
    );
