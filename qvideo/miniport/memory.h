#include "common.h"

#define QVMINI_TAG	'MMVQ'

PVOID AllocateMemory(
    ULONG uLength,
    PPFN_ARRAY *ppPfnArray
);

VOID FreeMemory(
    IN PVOID pMemory,
    IN PVOID pPfnArray
);

PVOID AllocateSection(
    ULONG uLength,
    HANDLE *phSection,
    PVOID *ppSectionObject,
    PVOID *ppMdl,
    PPFN_ARRAY *ppPfnArray,
    OPTIONAL HANDLE *phDirtySection,
    OPTIONAL PVOID *ppDirtySectionObject,
    OPTIONAL PVOID *ppDirtySectionMemory
);

VOID FreeSection(
    HANDLE hSection,
    PVOID pSectionObject,
    PVOID pMdl,
    PVOID BaseAddress,
    PVOID pPfnArray,
    OPTIONAL HANDLE hDirtySection,
    OPTIONAL PVOID pDirtySectionObject,
    OPTIONAL PVOID pDirtySectionMemory
);
