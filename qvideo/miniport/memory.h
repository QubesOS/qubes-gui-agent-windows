#include "common.h"

#define QVMINI_TAG	'MMVQ'

void *AllocateMemory(
    ULONG uLength,
    PFN_ARRAY **ppPfnArray
    );

VOID FreeMemory(
    IN void *pMemory,
    IN void *pPfnArray
    );

PVOID AllocateSection(
    ULONG uLength,
    HANDLE *phSection,
    void **ppSectionObject,
    void **ppMdl,
    PFN_ARRAY **ppPfnArray,
    OPTIONAL HANDLE *phDirtySection,
    OPTIONAL void **ppDirtySectionObject,
    OPTIONAL void **ppDirtySectionMemory
    );

VOID FreeSection(
    HANDLE hSection,
    void *pSectionObject,
    void *pMdl,
    void *BaseAddress,
    void *pPfnArray,
    OPTIONAL HANDLE hDirtySection,
    OPTIONAL void *pDirtySectionObject,
    OPTIONAL void *pDirtySectionMemory
    );
