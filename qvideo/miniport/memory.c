#include "ddk_video.h"
#include "ntstrsafe.h"
#include "memory.h"

NTSTATUS CreateAndMapSection(
	ULONG uSize,
	PHANDLE phSection,
	PVOID * pSectionObject,
	PVOID * pBaseAddress
)
{
	SIZE_T ViewSize = 0;
	NTSTATUS Status;
	HANDLE hSection;
	PVOID BaseAddress = NULL;
	OBJECT_ATTRIBUTES ObjectAttributes;
	UNICODE_STRING usSectionName;
	LARGE_INTEGER Size;
	PVOID SectionObject;
	WCHAR	SectionName[100];

	if (!uSize || !phSection || !pSectionObject || !pBaseAddress)
		return STATUS_INVALID_PARAMETER;

	*pSectionObject = NULL;
	*pBaseAddress = NULL;

	RtlStringCchPrintfW(SectionName, RTL_NUMBER_OF(SectionName), L"\\BaseNamedObjects\\QubesSharedMemory_%x", uSize);

	RtlInitUnicodeString(&usSectionName, SectionName);
	InitializeObjectAttributes(&ObjectAttributes, &usSectionName, OBJ_KERNEL_HANDLE, NULL, NULL);

	Size.HighPart = 0;
	Size.LowPart = uSize;

	ViewSize = uSize;

	Status = ZwCreateSection(&hSection, SECTION_ALL_ACCESS, &ObjectAttributes, &Size, PAGE_READWRITE, SEC_COMMIT, NULL);
	if (!NT_SUCCESS(Status)) {
		VideoDebugPrint((0, __FUNCTION__ ": ZwCreateSection() failed with status 0x%X\n", Status));
		return Status;
	}

	Status = ObReferenceObjectByHandle(hSection, SECTION_ALL_ACCESS, NULL, KernelMode, &SectionObject, NULL);
	if (!NT_SUCCESS(Status)) {
		VideoDebugPrint((0, __FUNCTION__ ": ObReferenceObjectByHandle() failed with status 0x%X\n", Status));
		ZwClose(hSection);
		return Status;
	}

	Status = MmMapViewInSystemSpace(SectionObject, &BaseAddress, &ViewSize);
	if (!NT_SUCCESS(Status)) {
		VideoDebugPrint((0, __FUNCTION__ ": MmMapViewInSystemSpace() failed with status 0x%X\n", Status));
		ObDereferenceObject(SectionObject);
		ZwClose(hSection);
		return Status;
	}

	*pBaseAddress = BaseAddress;
	*phSection = hSection;
	*pSectionObject = SectionObject;

	return Status;
}

VOID FreeSection(
	HANDLE hSection,
	PVOID SectionObject,
	PMDL pMdl,
	__in __drv_freesMem(Mem) PVOID BaseAddress
)
{
	if (pMdl) {
		MmUnlockPages(pMdl);
		IoFreeMdl(pMdl);
	}

	MmUnmapViewInSystemSpace(BaseAddress);
	ObDereferenceObject(SectionObject);
	ZwClose(hSection);
}

NTSTATUS GetBufferPfnArray(
	PVOID pVirtualAddress,
	ULONG uLength,
	PPFN_ARRAY pPfnArray,
	KPROCESSOR_MODE ProcessorMode,
	BOOLEAN bLockPages,
	PMDL * ppMdl
)
{
	NTSTATUS Status;
	PHYSICAL_ADDRESS PhysAddr;
	PMDL pMdl = NULL;
	PPFN_NUMBER pPfnNumber = NULL;
	ULONG i;

	if (!pVirtualAddress || !uLength || !pPfnArray)
		return STATUS_INVALID_PARAMETER;

	if (bLockPages && !ppMdl)
		return STATUS_INVALID_PARAMETER;

	if (ppMdl)
		*ppMdl = NULL;

	pMdl = IoAllocateMdl(pVirtualAddress, uLength, FALSE, FALSE, NULL);
	if (!pMdl) {
		VideoDebugPrint((0, __FUNCTION__ ": IoAllocateMdl() failed to allocate an MDL\n"));
		return STATUS_UNSUCCESSFUL;
	}

	Status = STATUS_SUCCESS;

	try {
		MmProbeAndLockPages(pMdl, ProcessorMode, IoWriteAccess);

	}
	except(EXCEPTION_EXECUTE_HANDLER) {
		VideoDebugPrint((0, __FUNCTION__ ": MmProbeAndLockPages() raised an exception, status 0x%08X\n", GetExceptionCode()));
		Status = STATUS_UNSUCCESSFUL;
	}

	if (NT_SUCCESS(Status)) {

		pPfnArray->uNumberOf4kPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(pMdl), MmGetMdlByteCount(pMdl));

		if (pPfnArray->uNumberOf4kPages > MAX_RETURNED_PFNS) {

			VideoDebugPrint((0, __FUNCTION__ ": Buffer is too large, needs %d PFNs to describe (returning first %d)\n",
					 pPfnArray->uNumberOf4kPages, MAX_RETURNED_PFNS));

			pPfnArray->uNumberOf4kPages = MAX_RETURNED_PFNS;
		}
		// sizeof(PFN_NUMBER) is 4 on x86 and 8 on x64.
		RtlCopyMemory(&pPfnArray->Pfn, MmGetMdlPfnArray(pMdl), sizeof(PFN_NUMBER) * pPfnArray->uNumberOf4kPages);

		if (!bLockPages) {
			MmUnlockPages(pMdl);
			IoFreeMdl(pMdl);
		} else
			*ppMdl = pMdl;
	} else
		IoFreeMdl(pMdl);

	return Status;

}

PVOID AllocateMemory(
	ULONG uLength,
	PPFN_ARRAY pPfnArray
)
{
	PVOID pMemory = NULL;
	NTSTATUS Status;

	if (!uLength || !pPfnArray)
		return NULL;

	uLength = ALIGN(uLength, PAGE_SIZE);

	pMemory = ExAllocatePoolWithTag(NonPagedPool, uLength, QVMINI_TAG);
	if (!pMemory)
		return NULL;

	Status = GetBufferPfnArray(pMemory, uLength, pPfnArray, KernelMode, FALSE, NULL);
	if (!NT_SUCCESS(Status)) {
		FreeMemory(pMemory);
		return NULL;
	}

	return pMemory;
}

VOID FreeMemory(
	__in __drv_freesMem(Mem) PVOID pMemory
)
{
	if (!pMemory)
		return;

	ExFreePoolWithTag(pMemory, QVMINI_TAG);
	return;

}

BOOLEAN GetUserBufferPfnArrayBool(
	PVOID pVirtualAddress,
	ULONG uLength,
	PPFN_ARRAY pPfnArray
)
{
	// Do not lock the user memory from kernel. Instead, lock it with VirtualLock() before calling GetUserBufferPfnArrayBool().
	return NT_SUCCESS(GetBufferPfnArray(pVirtualAddress, uLength, pPfnArray, UserMode, FALSE, NULL));
}

// Creates a named kernel mode section mapped in the system space, locks it, and returns its handle, a referenced section object, an MDL and a PFN list.
PVOID AllocateSection(
	ULONG uLength,
	PHANDLE phSection,
	PVOID * pSectionObject,
	PVOID * ppMdl,
	PPFN_ARRAY pPfnArray
)
{
	NTSTATUS Status;
	HANDLE hSection;
	PVOID SectionObject = NULL;
	PVOID BaseAddress = NULL;
	PMDL pMdl = NULL;

	if (!uLength || !phSection || !pSectionObject || !ppMdl || !pPfnArray)
		return NULL;

	*phSection = NULL;
	*pSectionObject = NULL;
	*ppMdl = NULL;

	Status = CreateAndMapSection(uLength, &hSection, &SectionObject, &BaseAddress);
	if (!NT_SUCCESS(Status))
		return NULL;

	// Lock the section memory and return its MDL.
	Status = GetBufferPfnArray(BaseAddress, uLength, pPfnArray, KernelMode, TRUE, &pMdl);
	if (!NT_SUCCESS(Status)) {
		FreeSection(hSection, SectionObject, NULL, BaseAddress);
		return NULL;
	}

	*phSection = hSection;
	*pSectionObject = SectionObject;
	*ppMdl = pMdl;

	return BaseAddress;
}
