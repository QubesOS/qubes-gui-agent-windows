#include "common.h"

#define QVMINI_TAG	'MMVQ'

PVOID AllocateMemory(
	ULONG uLength,
	PPFN_ARRAY pPfnArray
);

VOID FreeMemory(
	PVOID pMemory
);

PVOID AllocateSection(
	ULONG uLength,
	PHANDLE phSection,
	PVOID * pSectionObject,
	PVOID * ppMdl,
	PPFN_ARRAY pPfnArray
);

VOID FreeSection(
	HANDLE hSection,
	PVOID SectionObject,
	PVOID pMdl,
	__in __drv_freesMem(Mem) PVOID BaseAddress
);
