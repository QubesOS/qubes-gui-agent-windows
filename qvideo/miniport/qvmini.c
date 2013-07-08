#include "qvmini.h"

#if DBG
VOID QubesVideoNotImplemented(
	__in char *s
)
{
	VideoDebugPrint((0, "QubesVideo: Not used '%s'.\n", s));
}
#else
# define	QubesVideoNotImplemented(arg)
#endif

BOOLEAN QubesVideoResetHW(
	PVOID HwDeviceExtension,
	ULONG Columns,
	ULONG Rows
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);
	UNREFERENCED_PARAMETER(Columns);
	UNREFERENCED_PARAMETER(Rows);

	QubesVideoNotImplemented("QubesVideoResetHW");

	return TRUE;
}

VP_STATUS QubesVideoGetPowerState(
	PVOID HwDeviceExtension,
	ULONG HwId,
	PVIDEO_POWER_MANAGEMENT VideoPowerControl
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);
	UNREFERENCED_PARAMETER(HwId);
	UNREFERENCED_PARAMETER(VideoPowerControl);

	QubesVideoNotImplemented("QubesVideoGetPowerState");

	return NO_ERROR;
}

VP_STATUS QubesVideoSetPowerState(
	PVOID HwDeviceExtension,
	ULONG HwId,
	PVIDEO_POWER_MANAGEMENT VideoPowerControl
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);
	UNREFERENCED_PARAMETER(HwId);
	UNREFERENCED_PARAMETER(VideoPowerControl);

	QubesVideoNotImplemented("QubesVideoSetPowerState");

	return NO_ERROR;
}

VP_STATUS QubesVideoGetChildDescriptor(
	IN PVOID HwDeviceExtension,
	IN PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
	OUT PVIDEO_CHILD_TYPE pChildType,
	OUT PVOID pChildDescriptor,
	OUT PULONG pUId,
	OUT PULONG pUnused
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);
	UNREFERENCED_PARAMETER(ChildEnumInfo);
	UNREFERENCED_PARAMETER(pChildType);
	UNREFERENCED_PARAMETER(pChildDescriptor);
	UNREFERENCED_PARAMETER(pUId);
	UNREFERENCED_PARAMETER(pUnused);

	QubesVideoNotImplemented("QubesVideoGetChildDescriptor");

	return ERROR_NO_MORE_DEVICES;
}

VP_STATUS __checkReturn QubesVideoFindAdapter(
	__in PVOID HwDeviceExtension,
	__in PVOID HwContext,
	__in PWSTR ArgumentString,
	__inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) PVIDEO_PORT_CONFIG_INFO ConfigInfo,
	__out PUCHAR Again
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);
	UNREFERENCED_PARAMETER(HwContext);
	UNREFERENCED_PARAMETER(ArgumentString);
	UNREFERENCED_PARAMETER(ConfigInfo);
	UNREFERENCED_PARAMETER(Again);

	VideoDebugPrint((0, "QubesVideoFindAdapter Called.\n"));

	return NO_ERROR;
}

BOOLEAN QubesVideoInitialize(
	PVOID HwDeviceExtension
)
{
	UNREFERENCED_PARAMETER(HwDeviceExtension);

	VideoDebugPrint((0, "QubesVideoInitialize Called.\n"));

	return TRUE;
}

BOOLEAN QubesVideoStartIO(
	PVOID HwDeviceExtension,
	PVIDEO_REQUEST_PACKET RequestPacket
)
{
	PQVMINI_ALLOCATE_MEMORY pQvminiAllocateMemory = NULL;
	PQVMINI_ALLOCATE_MEMORY_RESPONSE pQvminiAllocateMemoryResponse = NULL;
	PQVMINI_FREE_MEMORY pQvminiFreeMemory = NULL;
	ULONG uLength;

	UNREFERENCED_PARAMETER(HwDeviceExtension);

	RequestPacket->StatusBlock->Status = 0;
	RequestPacket->StatusBlock->Information = 0;

	VideoDebugPrint((0, "QubesVideoStartIO Called.\n"));

	switch (RequestPacket->IoControlCode) {
	case IOCTL_QVMINI_ALLOCATE_MEMORY:

		if (RequestPacket->InputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY)) {
			RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
			RequestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY);
			break;
		}

		if (RequestPacket->OutputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE)) {
			RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
			RequestPacket->StatusBlock->Information = 0;
			break;
		}

		pQvminiAllocateMemory = RequestPacket->InputBuffer;
		pQvminiAllocateMemoryResponse = RequestPacket->OutputBuffer;

		uLength = pQvminiAllocateMemory->uLength;

		pQvminiAllocateMemoryResponse->pVirtualAddress = AllocateMemory(pQvminiAllocateMemory->uLength, &pQvminiAllocateMemoryResponse->PfnArray);

		if (!pQvminiAllocateMemoryResponse->pVirtualAddress) {

			VideoDebugPrint((0, "AllocateMemory(%d) failed.\n", uLength));

			RequestPacket->StatusBlock->Status = ERROR_NOT_ENOUGH_MEMORY;
			RequestPacket->StatusBlock->Information = 0;
		} else {

			VideoDebugPrint((0, "AllocateMemory(%d) succeeded (%p).\n", uLength, pQvminiAllocateMemoryResponse->pVirtualAddress));

			RequestPacket->StatusBlock->Status = NO_ERROR;
			RequestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE);
		}
		break;

	case IOCTL_QVMINI_FREE_MEMORY:

		if (RequestPacket->InputBufferLength < sizeof(QVMINI_FREE_MEMORY)) {
			RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
			RequestPacket->StatusBlock->Information = sizeof(QVMINI_FREE_MEMORY);
			break;
		}

		pQvminiFreeMemory = RequestPacket->InputBuffer;

		VideoDebugPrint((0, "FreeMemory(%p).\n", pQvminiFreeMemory->pVirtualAddress));

		FreeMemory(pQvminiFreeMemory->pVirtualAddress);

		RequestPacket->StatusBlock->Status = NO_ERROR;
		RequestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE);
		break;

	default:
		RequestPacket->StatusBlock->Status = ERROR_INVALID_FUNCTION;
		break;
	}

	return TRUE;
}

ULONG DriverEntry(
	PVOID Context1,
	PVOID Context2
)
{

	VIDEO_HW_INITIALIZATION_DATA hwInitData;
	ULONG initializationStatus;

	VideoDebugPrint((0, "Qubes Video Driver VideoPort [Driver Entry]\n"));

	// Zero out structure.

	VideoPortZeroMemory(&hwInitData, sizeof(VIDEO_HW_INITIALIZATION_DATA));

	// Specify sizes of structure and extension.

	hwInitData.HwInitDataSize = sizeof(VIDEO_HW_INITIALIZATION_DATA);

	// Set entry points.

	hwInitData.HwFindAdapter = &QubesVideoFindAdapter;
	hwInitData.HwInitialize = &QubesVideoInitialize;
	hwInitData.HwStartIO = &QubesVideoStartIO;
	hwInitData.HwResetHw = &QubesVideoResetHW;
	hwInitData.HwGetPowerState = &QubesVideoGetPowerState;
	hwInitData.HwSetPowerState = &QubesVideoSetPowerState;
	hwInitData.HwGetVideoChildDescriptor = &QubesVideoGetChildDescriptor;

	hwInitData.HwLegacyResourceList = NULL;
	hwInitData.HwLegacyResourceCount = 0;

	// no device extension necessary
	hwInitData.HwDeviceExtensionSize = 0;
	hwInitData.AdapterInterfaceType = 0;

	initializationStatus = VideoPortInitialize(Context1, Context2, &hwInitData, NULL);

	return initializationStatus;

}				// end DriverEntry()
