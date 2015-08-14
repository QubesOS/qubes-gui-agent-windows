#include "common.h"

#define QVMINI_TAG	'PMVQ'

typedef struct _QVM_BUFFER
{
    LIST_ENTRY ListEntry;
    PVOID KernelVa;
    ULONG OriginalSize;
    ULONG AlignedSize;
    PVOID PfnUserVa;
    PVOID PfnMdl; // miniport headers don't define MDLs...
    PVOID Process; // ...nor EPROCESS :/
    PPFN_ARRAY PfnArray;
    ULONG PfnArraySize;
} QVM_BUFFER, *PQVM_BUFFER;

/**
* @brief Allocate a non-paged buffer and get its PFN list.
* @param Size Required size. Allocation will be aligned to PAGE_SIZE.
* @return Buffer descriptor.
*/
PQVM_BUFFER QvmAllocateBuffer(
    __in ULONG Size
    );

/**
* @brief Free a buffer allocated by QvmAllocateBuffer.
* @param Buffer Buffer descriptor.
*/
VOID QvmFreeBuffer(
    __inout PQVM_BUFFER Buffer
    );

/**
* @brief Map a buffer's pfn array into the current process.
* @param Buffer Buffer descriptor.
* @return NTSTATUS.
*/
ULONG QvmMapBufferPfns(
    __inout PQVM_BUFFER Buffer
    );

/**
* @brief Unmap a buffer's pfn array from the current process.
* @param Buffer Buffer descriptor.
* @return NTSTATUS.
*/
ULONG QvmUnmapBufferPfns(
    __inout PQVM_BUFFER Buffer
    );
