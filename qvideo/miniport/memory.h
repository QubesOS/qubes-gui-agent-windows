#include "common.h"

#define QVMINI_TAG	'MMVQ'

void *AllocateMemory(
    IN ULONG size,
    OUT PFN_ARRAY **pfnArray
    );

void FreeMemory(
    IN void *memory,
    IN void *pfnArray OPTIONAL
    );

// Creates a named kernel mode section mapped in the system space, locks it,
// and returns its handle, a referenced section object, an MDL and a PFN list.
void *AllocateSection(
    IN ULONG size,
    OUT HANDLE *section,
    OUT void **sectionObject,
    OUT void **mdl, // MDL**, but good luck including required headers for the type
    OUT PFN_ARRAY **pfnArray,
    OUT HANDLE *dirtySection OPTIONAL,
    OUT void **dirtySectionObject OPTIONAL,
    OUT void **dirtySectionMemory OPTIONAL
    );

VOID FreeSection(
    IN HANDLE section,
    IN void *sectionObject,
    IN void *mdl, // MDL*, but good luck including required headers for the type
    IN void *baseAddress,
    IN void *pfnArray,
    IN HANDLE dirtySection OPTIONAL,
    IN void *dirtySectionObject OPTIONAL,
    IN void *dirtySectionMemory OPTIONAL
    );
