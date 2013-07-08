#include "dderror.h"
#include "devioctl.h"
#include "miniport.h"
#include "ntddvdeo.h"
#include "video.h"

#include "memory.h"

VP_STATUS __checkReturn QubesVideoFindAdapter(
	__in PVOID HwDeviceExtension,
	__in PVOID HwContext,
	__in PWSTR ArgumentString,
	__inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) PVIDEO_PORT_CONFIG_INFO ConfigInfo,
	__out PUCHAR Again
);

BOOLEAN __checkReturn QubesVideoInitialize(
	__in PVOID HwDeviceExtension
);

BOOLEAN __checkReturn QubesVideoStartIO(
	__in PVOID HwDeviceExtension,
	__in_bcount(sizeof(VIDEO_REQUEST_PACKET)) PVIDEO_REQUEST_PACKET RequestPacket
);

ULONG __checkReturn DriverEntry(
	__in PVOID Context1,
	__in PVOID Context2
);
