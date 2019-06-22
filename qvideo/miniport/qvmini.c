/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "qvmini.h"

#if DBG
VOID QubesVideoNotImplemented(
    __in const char *s
    )
{
    VideoDebugPrint((0, "[QVMINI] Not implemented: %s\n", s));
}
#else
# define	QubesVideoNotImplemented(arg)
#endif

BOOLEAN HwVidResetHW(
    void *hwDeviceExtension,
    ULONG columns,
    ULONG rows
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(columns);
    UNREFERENCED_PARAMETER(rows);

    QubesVideoNotImplemented(__FUNCTION__);

    return TRUE;
}

VP_STATUS HwVidGetPowerState(
    void *hwDeviceExtension,
    ULONG hwId,
    VIDEO_POWER_MANAGEMENT *videoPowerControl
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(hwId);
    UNREFERENCED_PARAMETER(videoPowerControl);

    QubesVideoNotImplemented(__FUNCTION__);

    return NO_ERROR;
}

VP_STATUS HwVidSetPowerState(
    void *hwDeviceExtension,
    ULONG hwId,
    VIDEO_POWER_MANAGEMENT *videoPowerControl
    )
{
    UNREFERENCED_PARAMETER(hwDeviceExtension);
    UNREFERENCED_PARAMETER(hwId);
    UNREFERENCED_PARAMETER(videoPowerControl);

    QubesVideoNotImplemented(__FUNCTION__);

    return NO_ERROR;
}

VP_STATUS HwVidGetChildDescriptor(
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

    QubesVideoNotImplemented(__FUNCTION__);

    return ERROR_NO_MORE_DEVICES;
}

VP_STATUS __checkReturn HwVidFindAdapter(
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

    QubesVideoNotImplemented(__FUNCTION__);

    return NO_ERROR;
}

BOOLEAN HwVidInitialize(
    PVOID DeviceExtension
    )
{
    PQVMINI_DX dx = (PQVMINI_DX)DeviceExtension;

    VideoDebugPrint((0, QFN("start\n")));
    // FIXME: when to perform cleanup? video miniport drivers don't have any "unload" callbacks... set power state most likely.
    VideoPortCreateSpinLock(DeviceExtension, &dx->BufferListLock);
    InitializeListHead(&dx->BufferList);

    return TRUE;
}

// main entry for function->miniport API
BOOLEAN HwVidStartIO(
    PVOID DeviceExtension,
    VIDEO_REQUEST_PACKET *Vrp
    )
{
    PQVMINI_DX dx = (PQVMINI_DX)DeviceExtension;
    PQVM_BUFFER buffer;
    UCHAR oldIrql;

    Vrp->StatusBlock->Status = 0;
    Vrp->StatusBlock->Information = 0;

    VideoDebugPrint((0, QFN("code 0x%x\n"), Vrp->IoControlCode));

    switch (Vrp->IoControlCode)
    {
    case IOCTL_QVMINI_ALLOCATE_MEMORY:
    {
        PQVMINI_ALLOCATE_MEMORY input = NULL;
        PQVMINI_ALLOCATE_MEMORY_RESPONSE output = NULL;

        if (Vrp->InputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY))
        {
            Vrp->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            Vrp->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY);
            break;
        }

        if (Vrp->OutputBufferLength < sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE))
        {
            Vrp->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            Vrp->StatusBlock->Information = 0;
            break;
        }

        input = Vrp->InputBuffer;
        output = Vrp->OutputBuffer;

        buffer = QvmAllocateBuffer(input->Size);

        if (!buffer)
        {
            VideoDebugPrint((0, QFN("QvmAllocateBuffer(%lu) failed\n"), input->Size));

            Vrp->StatusBlock->Status = ERROR_NOT_ENOUGH_MEMORY;
            Vrp->StatusBlock->Information = 0;
        }
        else
        {
            output->KernelVa = buffer->KernelVa;
            output->PfnArray = buffer->PfnArray;

            VideoPortAcquireSpinLock(dx, dx->BufferListLock, &oldIrql);
            InsertTailList(&dx->BufferList, &buffer->ListEntry);
            VideoPortReleaseSpinLock(dx, dx->BufferListLock, oldIrql);
            VideoDebugPrint((0, QFN("Added buffer %p, kva %p\n"), buffer, buffer->KernelVa));

            Vrp->StatusBlock->Status = NO_ERROR;
            Vrp->StatusBlock->Information = sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE);
        }
        break;
    }

    case IOCTL_QVMINI_FREE_MEMORY:
    {
        PQVMINI_FREE_MEMORY input = NULL;
        PLIST_ENTRY node;

        if (Vrp->InputBufferLength < sizeof(QVMINI_FREE_MEMORY))
        {
            Vrp->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            Vrp->StatusBlock->Information = sizeof(QVMINI_FREE_MEMORY);
            break;
        }

        input = Vrp->InputBuffer;

        // find the buffer descriptor
        buffer = NULL;
        VideoPortAcquireSpinLock(dx, dx->BufferListLock, &oldIrql);
        node = dx->BufferList.Flink;
        while (node->Flink != dx->BufferList.Flink)
        {
            buffer = CONTAINING_RECORD(node, QVM_BUFFER, ListEntry);

            node = node->Flink;
            if (buffer->KernelVa != input->KernelVa)
                continue;

            break;
        }
        VideoPortReleaseSpinLock(dx, dx->BufferListLock, oldIrql);

        if (buffer && buffer->KernelVa == input->KernelVa)
        {
            VideoDebugPrint((0, QFN("freeing buffer %p, kva %p\n"), buffer, buffer->KernelVa));
            VideoPortAcquireSpinLock(dx, dx->BufferListLock, &oldIrql);
            RemoveEntryList(&buffer->ListEntry);
            VideoPortReleaseSpinLock(dx, dx->BufferListLock, oldIrql);
            QvmFreeBuffer(buffer);
            Vrp->StatusBlock->Status = NO_ERROR;
        }
        else
        {
            VideoDebugPrint((0, QFN("buffer for kva %p not found\n"), input->KernelVa));
            Vrp->StatusBlock->Status = ERROR_INVALID_PARAMETER;
        }

        Vrp->StatusBlock->Information = 0;
        break;
    }

    case IOCTL_QVMINI_MAP_PFNS:
    {
        PQVMINI_MAP_PFNS input = NULL;
        PQVMINI_MAP_PFNS_RESPONSE output = NULL;
        PLIST_ENTRY node;

        if (Vrp->InputBufferLength < sizeof(QVMINI_MAP_PFNS))
        {
            Vrp->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            Vrp->StatusBlock->Information = sizeof(QVMINI_MAP_PFNS);
            break;
        }

        input = Vrp->InputBuffer;
        output = Vrp->OutputBuffer;

        // find the buffer descriptor
        buffer = NULL;
        VideoPortAcquireSpinLock(dx, dx->BufferListLock, &oldIrql);
        node = dx->BufferList.Flink;
        while (node->Flink != dx->BufferList.Flink)
        {
            buffer = CONTAINING_RECORD(node, QVM_BUFFER, ListEntry);

            node = node->Flink;
            if (buffer->KernelVa != input->KernelVa)
                continue;

            break;
        }
        VideoPortReleaseSpinLock(dx, dx->BufferListLock, oldIrql);

        if (buffer && buffer->KernelVa == input->KernelVa)
        {
            VideoDebugPrint((0, QFN("mapping pfns %p of buffer %p, kva %p\n"), buffer->PfnArray, buffer, buffer->KernelVa));
            Vrp->StatusBlock->Status = QvmMapBufferPfns(buffer);
            output->UserVa = buffer->PfnUserVa;
            Vrp->StatusBlock->Information = sizeof(QVMINI_MAP_PFNS_RESPONSE);
        }
        else
        {
            VideoDebugPrint((0, QFN("buffer for kva %p not found\n"), input->KernelVa));
            Vrp->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            Vrp->StatusBlock->Information = 0;
        }

        break;
    }

    case IOCTL_QVMINI_UNMAP_PFNS:
    {
        PQVMINI_UNMAP_PFNS input = NULL;
        PLIST_ENTRY node;

        if (Vrp->InputBufferLength < sizeof(QVMINI_UNMAP_PFNS))
        {
            Vrp->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            Vrp->StatusBlock->Information = sizeof(QVMINI_UNMAP_PFNS);
            break;
        }

        input = Vrp->InputBuffer;

        // find the buffer descriptor
        buffer = NULL;
        VideoPortAcquireSpinLock(dx, dx->BufferListLock, &oldIrql);
        node = dx->BufferList.Flink;
        while (node->Flink != dx->BufferList.Flink)
        {
            buffer = CONTAINING_RECORD(node, QVM_BUFFER, ListEntry);

            node = node->Flink;
            if (buffer->KernelVa != input->KernelVa)
                continue;

            break;
        }
        VideoPortReleaseSpinLock(dx, dx->BufferListLock, oldIrql);

        if (buffer && buffer->KernelVa == input->KernelVa)
        {
            VideoDebugPrint((0, QFN("unmapping pfns %p of buffer %p, kva %p\n"), buffer->PfnArray, buffer, buffer->KernelVa));
            Vrp->StatusBlock->Status = QvmUnmapBufferPfns(buffer);
        }
        else
        {
            VideoDebugPrint((0, QFN("buffer for kva %p not found\n"), input->KernelVa));
            Vrp->StatusBlock->Status = ERROR_INVALID_PARAMETER;
        }

        Vrp->StatusBlock->Information = 0;
        break;
    }

    default:
        Vrp->StatusBlock->Status = ERROR_INVALID_FUNCTION;
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
    ULONG status;

    VideoDebugPrint((0, QFN("start\n")));

    // Zero out structure.
    VideoPortZeroMemory(&hwInitData, sizeof(VIDEO_HW_INITIALIZATION_DATA));

    // Specify sizes of structure and extension.
    hwInitData.HwInitDataSize = sizeof(VIDEO_HW_INITIALIZATION_DATA);

    // Set entry points.
    hwInitData.HwFindAdapter = &HwVidFindAdapter;
    hwInitData.HwInitialize = &HwVidInitialize;
    hwInitData.HwStartIO = &HwVidStartIO;
    hwInitData.HwResetHw = &HwVidResetHW;
    hwInitData.HwGetPowerState = &HwVidGetPowerState;
    hwInitData.HwSetPowerState = &HwVidSetPowerState;
    hwInitData.HwGetVideoChildDescriptor = &HwVidGetChildDescriptor;

    hwInitData.HwLegacyResourceList = NULL;
    hwInitData.HwLegacyResourceCount = 0;

    hwInitData.HwDeviceExtensionSize = sizeof(QVMINI_DX);
    hwInitData.AdapterInterfaceType = 0;

    status = VideoPortInitialize(Context1, Context2, &hwInitData, NULL);

    return status;
}
