#include "ddk_video.h"
#include "ntstrsafe.h"
#include "memory.h"

static NTSTATUS CreateAndMapSection(
    IN UNICODE_STRING sectionName,
    IN ULONG size,
    OUT HANDLE *section,
    OUT void **sectionObject,
    OUT void **baseAddress
    )
{
    SIZE_T viewSize = 0;
    NTSTATUS status;
    OBJECT_ATTRIBUTES objectAttributes;
    LARGE_INTEGER sizeLarge;

    if (!size || !section || !sectionObject || !baseAddress)
        return STATUS_INVALID_PARAMETER;

    *sectionObject = NULL;
    *baseAddress = NULL;

    InitializeObjectAttributes(&objectAttributes, &sectionName, OBJ_KERNEL_HANDLE, NULL, NULL);

    sizeLarge.HighPart = 0;
    sizeLarge.LowPart = size;

    viewSize = size;

    status = ZwCreateSection(section, SECTION_ALL_ACCESS, &objectAttributes, &sizeLarge, PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(status))
    {
        VideoDebugPrint((0, __FUNCTION__ ": ZwCreateSection() failed with status 0x%X\n", status));
        return status;
    }

    status = ObReferenceObjectByHandle(*section, SECTION_ALL_ACCESS, NULL, KernelMode, sectionObject, NULL);
    if (!NT_SUCCESS(status))
    {
        VideoDebugPrint((0, __FUNCTION__ ": ObReferenceObjectByHandle() failed with status 0x%X\n", status));
        ZwClose(*section);
        return status;
    }

    // FIXME: undocumented/unsupported function
    status = MmMapViewInSystemSpace(*sectionObject, baseAddress, &viewSize);
    if (!NT_SUCCESS(status))
    {
        VideoDebugPrint((0, __FUNCTION__ ": MmMapViewInSystemSpace() failed with status 0x%X\n", status));
        ObDereferenceObject(*sectionObject);
        ZwClose(*section);
        return status;
    }

    VideoDebugPrint((0, __FUNCTION__": section '%wZ': %p, addr %p\n",
        sectionName, *sectionObject, *baseAddress));

    return status;
}

static NTSTATUS GetBufferPfnArray(
    IN void *virtualAddress,
    IN ULONG size,
    OUT PFN_ARRAY **pfnArray OPTIONAL, // pfn array is allocated here
    IN KPROCESSOR_MODE processorMode,
    IN BOOLEAN lockPages,
    OUT MDL **bufferMdl OPTIONAL
    )
{
    NTSTATUS status;
    MDL *mdl = NULL;
    ULONG numberOfPages;

    if (!virtualAddress || !size)
        return STATUS_INVALID_PARAMETER;

    if (bufferMdl)
        *bufferMdl = NULL;

    mdl = IoAllocateMdl(virtualAddress, size, FALSE, FALSE, NULL);
    if (!mdl)
    {
        VideoDebugPrint((0, __FUNCTION__ ": IoAllocateMdl() failed to allocate an MDL\n"));
        return STATUS_UNSUCCESSFUL;
    }

    status = STATUS_SUCCESS;

    try
    {
        MmProbeAndLockPages(mdl, processorMode, IoWriteAccess);
    }
    except(EXCEPTION_EXECUTE_HANDLER)
    {
        VideoDebugPrint((0, __FUNCTION__ ": MmProbeAndLockPages() raised an exception, status 0x%08X\n", GetExceptionCode()));
        status = STATUS_UNSUCCESSFUL;
    }

    if (NT_SUCCESS(status))
    {
        if (pfnArray)
        {
            numberOfPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(mdl), MmGetMdlByteCount(mdl));
            // size of PFN_ARRAY
            *pfnArray = ExAllocatePoolWithTag(NonPagedPool, numberOfPages*sizeof(PFN_NUMBER) + sizeof(ULONG), QVMINI_TAG);

            if (!(*pfnArray))
                return STATUS_NO_MEMORY;

            (*pfnArray)->NumberOf4kPages = numberOfPages;
            RtlCopyMemory((*pfnArray)->Pfn, MmGetMdlPfnArray(mdl), numberOfPages*sizeof(PFN_NUMBER));
        }

        if (!lockPages)
        {
            MmUnlockPages(mdl);
            IoFreeMdl(mdl);
        }
        else
        {
            if (bufferMdl)
                *bufferMdl = mdl;
        }
    }
    else
        IoFreeMdl(mdl);

    return status;
}

void *AllocateMemory(
    IN ULONG size,
    OUT PFN_ARRAY **pfnArray
    )
{
    void *memory = NULL;
    NTSTATUS status;

    if (!size || !pfnArray)
        return NULL;

    size = ALIGN(size, PAGE_SIZE);

    memory = ExAllocatePoolWithTag(NonPagedPool, size, QVMINI_TAG);
    if (!memory)
        return NULL;

    VideoDebugPrint((0, __FUNCTION__": %p\n", memory));

    status = GetBufferPfnArray(memory, size, pfnArray, KernelMode, FALSE, NULL);
    if (!NT_SUCCESS(status))
    {
        FreeMemory(memory, NULL); // ppPfnArray is only allocated on success
        return NULL;
    }

    return memory;
}

void FreeMemory(
    IN void *memory,
    IN void *pfnArray OPTIONAL
    )
{
    if (!memory)
        return;

    VideoDebugPrint((0, __FUNCTION__": %p, pfn: %p\n", memory, pfnArray));

    ExFreePoolWithTag(memory, QVMINI_TAG);
    if (pfnArray)
        ExFreePoolWithTag(pfnArray, QVMINI_TAG);
    return;
}

// Creates a named kernel mode section mapped in the system space, locks it,
// and returns its handle, a referenced section object, an MDL and a PFN list.
void *AllocateSection(
    IN ULONG size,
    OUT HANDLE *section,
    OUT void **sectionObject,
    OUT MDL **mdl,
    OUT PFN_ARRAY **pfnArray,
    OUT HANDLE *dirtySection OPTIONAL,
    OUT void **dirtySectionObject OPTIONAL,
    OUT void **dirtySectionMemory OPTIONAL
    )
{
    NTSTATUS status;
    void *baseAddress = NULL;
    UNICODE_STRING sectionNameU;
    WCHAR sectionName[100];

    if (!size || !section || !sectionObject || !mdl || !pfnArray)
        return NULL;

    RtlStringCchPrintfW(sectionName, RTL_NUMBER_OF(sectionName), L"\\BaseNamedObjects\\QubesSharedMemory_%x", size);
    RtlInitUnicodeString(&sectionNameU, sectionName);

    status = CreateAndMapSection(sectionNameU, size, section, sectionObject, &baseAddress);
    if (!NT_SUCCESS(status))
        return NULL;

    // Lock the section memory and return its MDL.
    status = GetBufferPfnArray(baseAddress, size, pfnArray, KernelMode, TRUE, mdl);
    if (!NT_SUCCESS(status))
    {
        FreeSection(*section, *sectionObject, NULL, baseAddress, NULL, NULL, NULL, NULL);
        return NULL;
    }

    if (dirtySection && dirtySectionObject && dirtySectionMemory)
    {
        *dirtySection = NULL;
        *dirtySectionObject = NULL;
        *dirtySectionMemory = NULL;

        // struct header + bit array for dirty pages
        size = sizeof(QV_DIRTY_PAGES) + ((size / PAGE_SIZE) >> 3) + 1;
        RtlStringCchPrintfW(sectionName, RTL_NUMBER_OF(sectionName), L"\\BaseNamedObjects\\QvideoDirtyPages_%x", size);
        RtlInitUnicodeString(&sectionNameU, sectionName);

        status = CreateAndMapSection(sectionNameU, size, dirtySection, dirtySectionObject, dirtySectionMemory);
        if (!NT_SUCCESS(status))
            return NULL;

        // just lock, don't get PFNs
        status = GetBufferPfnArray(*dirtySectionMemory, size, NULL, KernelMode, TRUE, NULL);
        if (!NT_SUCCESS(status))
        {
            FreeSection(*section, *sectionObject, NULL, baseAddress, NULL, *dirtySection, *dirtySectionObject, *dirtySectionMemory);
            return NULL;
        }
    }
    return baseAddress;
}

void FreeSection(
    IN HANDLE section,
    IN void *sectionObject,
    IN MDL *mdl,
    IN void *baseAddress,
    IN void *pfnArray,
    OPTIONAL HANDLE dirtySection,
    OPTIONAL void *dirtySectionObject,
    OPTIONAL void *dirtySectionMemory
    )
{
    if (mdl)
    {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
    }

    VideoDebugPrint((0, __FUNCTION__": section %p, dirty %p\n", sectionObject, dirtySectionObject));
    MmUnmapViewInSystemSpace(baseAddress); // FIXME: undocumented/unsupported function
    ObDereferenceObject(sectionObject);
    ZwClose(section);

    if (pfnArray)
        ExFreePoolWithTag(pfnArray, QVMINI_TAG);

    if (dirtySection && dirtySectionObject && dirtySectionMemory)
    {
        MmUnmapViewInSystemSpace(dirtySectionMemory);
        ObDereferenceObject(dirtySectionObject);
        ZwClose(dirtySection);
    }
}
