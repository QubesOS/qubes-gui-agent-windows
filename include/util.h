#pragma once
#include <windows.h>

extern DWORD g_DisableCursor;

ULONG HideCursors(void);
ULONG DisableEffects(void);
HANDLE CreateNamedEvent(IN const WCHAR *name);
ULONG StartProcess(IN WCHAR *executable, OUT PHANDLE processHandle);
ULONG IncreaseProcessWorkingSetSize(SIZE_T uNewMinimumWorkingSetSize, SIZE_T uNewMaximumWorkingSetSize);
ULONG AttachToInputDesktop(void);
void PageToRect(ULONG uPageNumber, OUT PRECT pRect);
