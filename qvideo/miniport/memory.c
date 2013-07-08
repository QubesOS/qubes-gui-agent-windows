#include "ddk_video.h"
#include "memory.h"

NTSTATUS GetBufferPfnArray(
	PVOID pVirtualAddress,
	ULONG uLength,
	PPFN_ARRAY pPfnArray
)
{
	NTSTATUS Status;
	PHYSICAL_ADDRESS PhysAddr;
	PMDL pMdl = NULL;
	PPFN_NUMBER pPfnNumber = NULL;
	ULONG i;

	if (!pVirtualAddress || !uLength || !pPfnArray)
		return STATUS_INVALID_PARAMETER;

	pMdl = IoAllocateMdl(pVirtualAddress, uLength, FALSE, FALSE, NULL);
	if (!pMdl) {
		VideoDebugPrint((0, __FUNCTION__ ": IoAllocateMdl() failed to allocate an MDL\n"));
		return STATUS_UNSUCCESSFUL;
	}

	Status = STATUS_SUCCESS;

	try {
		MmProbeAndLockPages(pMdl, KernelMode, IoWriteAccess);

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

		MmUnlockPages(pMdl);
	}

	IoFreeMdl(pMdl);
	return Status;

}

PVOID AllocateMemory(
	ULONG uLength,
	PPFN_ARRAY pPfnArray
)
{
	PHYSICAL_ADDRESS HighestAcceptableAddress;
	PVOID pMemory = NULL;
	NTSTATUS Status;

	if (!uLength || !pPfnArray)
		return NULL;

	//
	// Memory can reside anywhere
	//

	HighestAcceptableAddress.LowPart = 0xFFFFFFFF;
	HighestAcceptableAddress.HighPart = 0xFFFFFFFF;

	pMemory = MmAllocateContiguousMemory(uLength, HighestAcceptableAddress);

	if (!pMemory)
		return NULL;

	Status = GetBufferPfnArray(pMemory, uLength, pPfnArray);
	if (!NT_SUCCESS(Status)) {
		MmFreeContiguousMemory(pMemory);
		return NULL;
	}

	return pMemory;
}

VOID FreeMemory(
	PVOID pMemory
)
{
	if (!pMemory)
		return;

	MmFreeContiguousMemory(pMemory);
	return;

}
