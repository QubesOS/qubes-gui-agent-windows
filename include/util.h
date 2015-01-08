#pragma once
#include <windows.h>

extern DWORD g_DisableCursor;

ULONG HideCursors(void);
ULONG DisableEffects(void);
HANDLE CreateNamedEvent(IN const WCHAR *name); // returns NULL on failure
HANDLE CreateNamedMailslot(IN const WCHAR *name); // returns NULL on failure
ULONG StartProcess(IN WCHAR *executable, OUT HANDLE *processHandle);
ULONG IncreaseProcessWorkingSetSize(IN SIZE_T minimumSize, IN SIZE_T maximumSize);
ULONG AttachToInputDesktop(void);
void PageToRect(ULONG pageNumber, OUT RECT *rect);
