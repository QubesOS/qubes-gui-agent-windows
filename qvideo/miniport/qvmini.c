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
    void *hwDeviceExtension,
    ULONG columns,
    ULONG rows
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(columns);
    UNREFERENCED_PARAMETER(rows);

    QubesVideoNotImplemented("QubesVideoResetHW");

    return TRUE;
}

VP_STATUS QubesVideoGetPowerState(
    void *hwDeviceExtension,
    ULONG hwId,
    VIDEO_POWER_MANAGEMENT *videoPowerControl
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(hwId);
    UNREFERENCED_PARAMETER(videoPowerControl);

    QubesVideoNotImplemented("QubesVideoGetPowerState");

    return NO_ERROR;
}

VP_STATUS QubesVideoSetPowerState(
    void *hwDeviceExtension,
    ULONG hwId,
    VIDEO_POWER_MANAGEMENT *videoPowerControl
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(hwId);
    UNREFERENCED_PARAMETER(videoPowerControl);

    QubesVideoNotImplemented("QubesVideoSetPowerState");

    return NO_ERROR;
}

VP_STATUS QubesVideoGetChildDescriptor(
    IN void *hwDeviceExtension,
    IN VIDEO_CHILD_ENUM_INFO *childEnumInfo,
    OUT VIDEO_CHILD_TYPE *childType,
    OUT void *childDescriptor,
    OUT ULONG *id,
    OUT ULONG *unused
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(childEnumInfo);
    UNREFERENCED_PARAMETER(childType);
    UNREFERENCED_PARAMETER(childDescriptor);
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(unused);

    QubesVideoNotImplemented("QubesVideoGetChildDescriptor");

    return ERROR_NO_MORE_DEVICES;
}

VP_STATUS __checkReturn QubesVideoFindAdapter(
    __in void *hwDeviceExtension,
    __in void *hwContext,
    __in WCHAR *argumentString,
    __inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) PVIDEO_PORT_CONFIG_INFO configInfo,
    __out UCHAR *again
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(hwContext);
    UNREFERENCED_PARAMETER(argumentString);
    UNREFERENCED_PARAMETER(configInfo);
    UNREFERENCED_PARAMETER(again);

    VideoDebugPrint((0, "QubesVideoFindAdapter Called.\n"));

    return NO_ERROR;
}

BOOLEAN QubesVideoInitialize(
    void *hwDeviceExtension
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);

    VideoDebugPrint((0, "QubesVideoInitialize Called.\n"));

    return TRUE;
}

// main entry for function->miniport API
BOOLEAN QubesVideoStartIO(
    void *hwDeviceExtension,
    VIDEO_REQUEST_PACKET *requestPacket
    )
{
    QVMINI_ALLOCATE_MEMORY *qvminiAllocateMemory = NULL;
    QVMINI_ALLOCATE_MEMORY_RESPONSE *qvminiAllocateMemoryResponse = NULL;
    QVMINI_FREE_MEMORY *qvminiFreeMemory = NULL;
    QVMINI_ALLOCATE_SECTION *qvminiAllocateSection = NULL;
    QVMINI_ALLOCATE_SECTION_RESPONSE *qvminiAllocateSectionResponse = NULL;
    QVMINI_FREE_SECTION *qvminiFreeSection = NULL;

    ULONG size;

    UNREFERENCED_PARAMETER(hwDeviceExtension);

    requestPacket->StatusBlock->Status = 0;
    requestPacket->StatusBlock->Information = 0;

    //  VideoDebugPrint((0, "QubesVideoStartIO Called.\n"));

    switch (requestPacket->IoControlCode)
    {
    case IOCTL_QVMINI_ALLOCATE_MEMORY:

        if (requestPacket->InputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY);
            break;
        }

        if (requestPacket->OutputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = 0;
            break;
        }

        qvminiAllocateMemory = requestPacket->InputBuffer;
        qvminiAllocateMemoryResponse = requestPacket->OutputBuffer;

        size = qvminiAllocateMemory->Size;

        qvminiAllocateMemoryResponse->VirtualAddress =
            AllocateMemory(qvminiAllocateMemory->Size, &qvminiAllocateMemoryResponse->PfnArray);

        if (!qvminiAllocateMemoryResponse->VirtualAddress)
        {
            VideoDebugPrint((0, "AllocateMemory(%lu) failed\n", size));

            requestPacket->StatusBlock->Status = ERROR_NOT_ENOUGH_MEMORY;
            requestPacket->StatusBlock->Information = 0;
        }
        else
        {
            // VideoDebugPrint((0, "AllocateMemory(%lu) succeeded (%p).\n", size, pQvminiAllocateMemoryResponse->pVirtualAddress));

            requestPacket->StatusBlock->Status = NO_ERROR;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE);
        }
        break;

    case IOCTL_QVMINI_FREE_MEMORY:

        if (requestPacket->InputBufferLength < sizeof(QVMINI_FREE_MEMORY))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_FREE_MEMORY);
            break;
        }

        qvminiFreeMemory = requestPacket->InputBuffer;

        // VideoDebugPrint((0, "FreeMemory(%p).\n", pQvminiFreeMemory->pVirtualAddress));

        FreeMemory(qvminiFreeMemory->VirtualAddress, qvminiFreeMemory->PfnArray);

        requestPacket->StatusBlock->Status = NO_ERROR;
        requestPacket->StatusBlock->Information = 0;
        break;

    case IOCTL_QVMINI_ALLOCATE_SECTION:

        if (requestPacket->InputBufferLength < sizeof(QVMINI_ALLOCATE_SECTION))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_SECTION);
            break;
        }

        if (requestPacket->OutputBufferLength < sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = 0;
            break;
        }

        qvminiAllocateSection = requestPacket->InputBuffer;
        qvminiAllocateSectionResponse = requestPacket->OutputBuffer;

        size = qvminiAllocateSection->Size;

        if (qvminiAllocateSection->UseDirtyBits)
        {
            qvminiAllocateSectionResponse->VirtualAddress =
                AllocateSection(qvminiAllocateSection->Size, &qvminiAllocateSectionResponse->Section,
                &qvminiAllocateSectionResponse->SectionObject, &qvminiAllocateSectionResponse->Mdl,
                &qvminiAllocateSectionResponse->PfnArray,
                &qvminiAllocateSectionResponse->DirtySection,
                &qvminiAllocateSectionResponse->DirtySectionObject,
                &qvminiAllocateSectionResponse->DirtyPages);
        }
        else // don't alloc dirty bits section
        {
            qvminiAllocateSectionResponse->VirtualAddress =
                AllocateSection(qvminiAllocateSection->Size, &qvminiAllocateSectionResponse->Section,
                &qvminiAllocateSectionResponse->SectionObject, &qvminiAllocateSectionResponse->Mdl,
                &qvminiAllocateSectionResponse->PfnArray,
                NULL, NULL, NULL
                );
            qvminiAllocateSectionResponse->DirtySectionObject = NULL;
            qvminiAllocateSectionResponse->DirtySection = NULL;
            qvminiAllocateSectionResponse->DirtyPages = NULL;
        }

        if (!qvminiAllocateSectionResponse->VirtualAddress)
        {
            VideoDebugPrint((0, "AllocateSection(%d) failed.\n", size));

            requestPacket->StatusBlock->Status = ERROR_NOT_ENOUGH_MEMORY;
            requestPacket->StatusBlock->Information = 0;
        }
        else
        {
            // VideoDebugPrint((0, "AllocateSection(%lu) succeeded (%p).\n", uLength, pQvminiAllocateSectionResponse->pVirtualAddress));

            requestPacket->StatusBlock->Status = NO_ERROR;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE);
        }
        break;

    case IOCTL_QVMINI_FREE_SECTION:

        if (requestPacket->InputBufferLength < sizeof(QVMINI_FREE_SECTION))
        {
            requestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            requestPacket->StatusBlock->Information = sizeof(QVMINI_FREE_SECTION);
            break;
        }

        qvminiFreeSection = requestPacket->InputBuffer;

        // VideoDebugPrint((0, "FreeMemory(%p).\n", pQvminiFreeSection->pVirtualAddress));

        FreeSection(qvminiFreeSection->Section, qvminiFreeSection->SectionObject,
            qvminiFreeSection->Mdl, qvminiFreeSection->VirtualAddress,
            qvminiFreeSection->PfnArray,
            qvminiFreeSection->DirtySection, qvminiFreeSection->DirtySectionObject,
            qvminiFreeSection->DirtyPages);

        requestPacket->StatusBlock->Status = NO_ERROR;
        requestPacket->StatusBlock->Information = 0;
        break;

    default:
        requestPacket->StatusBlock->Status = ERROR_INVALID_FUNCTION;
        break;
    }

    return TRUE;
}

ULONG DriverEntry(
    void *Context1,
    void *Context2
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
}
