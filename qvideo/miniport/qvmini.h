#pragma warning(disable: 4201)
#include "dderror.h"
#include "devioctl.h"
#include "miniport.h"
#include "ntddvdeo.h"
#include "video.h"

#include "memory.h"

VP_STATUS __checkReturn QubesVideoFindAdapter(
    __in void *HwDeviceExtension,
    __in void *HwContext,
    __in WCHAR *ArgumentString,
    __inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) VIDEO_PORT_CONFIG_INFO *ConfigInfo,
    __out UCHAR *Again
    );

BOOLEAN __checkReturn QubesVideoInitialize(
    __in void *HwDeviceExtension
    );

BOOLEAN __checkReturn QubesVideoStartIO(
    __in void *HwDeviceExtension,
    __in_bcount(sizeof(VIDEO_REQUEST_PACKET)) VIDEO_REQUEST_PACKET *RequestPacket
    );

ULONG __checkReturn DriverEntry(
    __in void *Context1,
    __in void *Context2
    );
