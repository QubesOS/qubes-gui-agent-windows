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

#include "ddk_video.h"
#include <ntstrsafe.h>
#include "memory.h"

#define QFN(x) "[QVMINI] %s: " x, __FUNCTION__

/**
 * @brief Initialize PFN array for the specified buffer.
 * @param Buffer Buffer descriptor. Uninitialized fields must be zeroed.
 * @return NTSTATUS.
 */
static NTSTATUS GetBufferPfnArray(
    __inout PQVM_BUFFER Buffer
    )
{
    NTSTATUS status = STATUS_NO_MEMORY;
    ULONG numberPages;
    PMDL mdl = NULL;

    mdl = IoAllocateMdl(Buffer->KernelVa, Buffer->AlignedSize, FALSE, FALSE, NULL);
    if (!mdl)
    {
        VideoDebugPrint((0, QFN("IoAllocateMdl(buffer) failed\n")));
        goto cleanup;
    }

    MmBuildMdlForNonPagedPool(mdl);

    numberPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(Buffer->KernelVa, MmGetMdlByteCount(mdl));
    Buffer->PfnArraySize = numberPages*sizeof(PFN_NUMBER) + sizeof(ULONG);
    VideoDebugPrint((0, QFN("buffer %p, PfnArraySize: %lu, aligned: %lu, number pages: %lu\n"),
        Buffer, Buffer->PfnArraySize, ALIGN(Buffer->PfnArraySize, PAGE_SIZE), numberPages));

    // align to page size because it'll be mapped to user mode
    Buffer->PfnArraySize = ALIGN(Buffer->PfnArraySize, PAGE_SIZE);
    Buffer->PfnArray = ExAllocatePoolWithTag(NonPagedPool, Buffer->PfnArraySize, QVMINI_TAG);

    if (!Buffer->PfnArray)
    {
        VideoDebugPrint((0, QFN("pfn array allocation failed\n")));
        goto cleanup;
    }

    RtlZeroMemory(Buffer->PfnArray, Buffer->PfnArraySize);
    Buffer->PfnArray->NumberOf4kPages = numberPages;
    // copy actual pfns
    memcpy(Buffer->PfnArray->Pfn, MmGetMdlPfnArray(mdl), numberPages*sizeof(PFN_NUMBER));
    IoFreeMdl(mdl);
    mdl = NULL;

    Buffer->PfnMdl = IoAllocateMdl(Buffer->PfnArray, Buffer->PfnArraySize, FALSE, FALSE, NULL);
    if (!Buffer->PfnMdl)
    {
        VideoDebugPrint((0, QFN("IoAllocateMdl(pfns) failed\n")));
        goto cleanup;
    }

    MmBuildMdlForNonPagedPool(Buffer->PfnMdl);
    status = STATUS_SUCCESS;

cleanup:
    if (!NT_SUCCESS(status))
    {
        if (mdl)
            IoFreeMdl(mdl);
        if (Buffer->PfnMdl)
            IoFreeMdl(Buffer->PfnMdl);
        if (Buffer->PfnArray)
            ExFreePoolWithTag(Buffer->PfnArray, QVMINI_TAG);
    }
    return status;
}

/**
 * @brief Allocate a non-paged buffer and get its PFN list.
 * @param Size Required size. Allocation will be aligned to PAGE_SIZE.
 * @return Buffer descriptor.
 */
PQVM_BUFFER QvmAllocateBuffer(
    __in ULONG Size
    )
{
    PQVM_BUFFER buffer = NULL;
    NTSTATUS status = STATUS_NO_MEMORY;

    buffer = ExAllocatePoolWithTag(NonPagedPool, sizeof(QVM_BUFFER), QVMINI_TAG);
    if (!buffer)
    {
        VideoDebugPrint((0, QFN("allocate buffer failed\n")));
        goto cleanup;
    }

    RtlZeroMemory(buffer, sizeof(QVM_BUFFER));
    buffer->OriginalSize = Size;
    // Size must be page-aligned because this buffer will be mapped into user space.
    // Mapping is page-granular and we don't want to leak any kernel data.
    // Only allocations sized >= PAGE_SIZE are guaranteed to start at a page-aligned address.
    buffer->AlignedSize = ALIGN(buffer->OriginalSize, PAGE_SIZE);

    buffer->KernelVa = ExAllocatePoolWithTag(NonPagedPool, buffer->AlignedSize, QVMINI_TAG);
    if (!buffer->KernelVa)
    {
        VideoDebugPrint((0, QFN("allocate buffer data (%lu) failed\n"), buffer->AlignedSize));
        goto cleanup;
    }

    status = GetBufferPfnArray(buffer);
    if (!NT_SUCCESS(status))
    {
        VideoDebugPrint((0, QFN("GetBufferPfnArray (%p) failed: 0x%x\n"), buffer, status));
        goto cleanup;
    }

    VideoDebugPrint((0, QFN("buffer %p, kva %p, aligned size %lu, pfn array %p, pfn array size %lu\n"),
        buffer, buffer->KernelVa, buffer->AlignedSize, buffer->PfnArray, buffer->PfnArraySize));

    status = STATUS_SUCCESS;

cleanup:
    if (!NT_SUCCESS(status))
    {
        RtlZeroMemory(buffer, sizeof(QVM_BUFFER));
        ExFreePoolWithTag(buffer, QVMINI_TAG);
        buffer = NULL;
    }
    return buffer;
}

/**
 * @brief Free a buffer allocated by QvmAllocateBuffer. Unmap pfns from user space if mapped.
 * @param Buffer Buffer descriptor.
 */
VOID QvmFreeBuffer(
    __inout PQVM_BUFFER Buffer
    )
{
    VideoDebugPrint((0, QFN("buffer %p, kva %p, aligned size %lu, pfn array %p, pfn array size %lu\n"),
        Buffer, Buffer->KernelVa, Buffer->AlignedSize, Buffer->PfnArray, Buffer->PfnArraySize));

    if (Buffer->PfnUserVa)
        QvmUnmapBufferPfns(Buffer);

    IoFreeMdl(Buffer->PfnMdl);
    RtlZeroMemory(Buffer->PfnArray, Buffer->PfnArraySize);
    ExFreePoolWithTag(Buffer->PfnArray, QVMINI_TAG);
    RtlZeroMemory(Buffer, sizeof(QVM_BUFFER));
    ExFreePoolWithTag(Buffer, QVMINI_TAG);
}

/**
 * @brief Map a buffer's pfn array into the current process.
 * @param Buffer Buffer descriptor.
 * @return NTSTATUS.
 */
ULONG QvmMapBufferPfns(
    __inout PQVM_BUFFER Buffer
    )
{
    NTSTATUS status;

    ASSERT(!Buffer->Process);
    ASSERT(!Buffer->PfnUserVa);

    VideoDebugPrint((0, QFN("mapping pfns of buffer %p, kva %p\n"), Buffer, Buffer->KernelVa));
#pragma prefast(suppress: 6320) // we want to catch all exceptions
    __try
    {
        Buffer->PfnUserVa = MmMapLockedPagesSpecifyCache(Buffer->PfnMdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
        // make the map read only
        status = MmProtectMdlSystemAddress(Buffer->PfnMdl, PAGE_READONLY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        VideoDebugPrint((0, QFN("exception 0x%x\n"), status));
        goto cleanup;
    }

    Buffer->Process = PsGetCurrentProcess();
    VideoDebugPrint((0, QFN("PfnUserVa %p, process %p\n"), Buffer->PfnUserVa, Buffer->Process));
    status = STATUS_SUCCESS;

cleanup:
    return status;
}

/**
* @brief Unmap a buffer's pfn array from the current process.
* @param Buffer Buffer descriptor.
* @return NTSTATUS.
*/
ULONG QvmUnmapBufferPfns(
    __inout PQVM_BUFFER Buffer
    )
{
    NTSTATUS status;

    ASSERT(Buffer->PfnUserVa && Buffer->Process);
    ASSERT(Buffer->Process == PsGetCurrentProcess());

    VideoDebugPrint((0, QFN("unmapping pfns of buffer %p, kva %p\n"), Buffer, Buffer->KernelVa));
#pragma prefast(suppress: 6320) // we want to catch all exceptions
    __try
    {
        MmUnmapLockedPages(Buffer->PfnUserVa, Buffer->PfnMdl);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        VideoDebugPrint((0, QFN("exception 0x%x\n"), status));
        goto cleanup;
    }

    Buffer->PfnUserVa = NULL;
    Buffer->Process = NULL;
    status = STATUS_SUCCESS;

cleanup:
    return status;
}
