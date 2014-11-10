#pragma once
#include <Windows.h>

// Name of the mailslot used for hook->gui agent communication.
#define HOOK_IPC_NAME L"\\\\.\\mailslot\\QubesGuiHookIPC"

#define HOOK_DLL_NAME_64    L"QubesGuiHook64.dll"
#define HOOK_DLL_NAME_32    L"QubesGuiHook32.dll"
#define HOOK_SERVER_NAME_32 L"QGuiHookServer32.exe"

// This needs to be bitness-agnostic, which is a giant pain,
// because we can't just include winuser's structs.
typedef struct _QH_MESSAGE
{
    UINT32 HookId; // WH_*
    UINT32 Message; // WM_*
    UINT64 WindowHandle;
    UINT64 wParam; // message's wparam
    UINT64 lParam; // message's lparam
    BOOL Is64bit;

    // These fields are message-dependent.
    UINT64 Flags;
    int X;
    int Y;
    int Width;
    int Height;
    UINT32 Style;
    UINT32 ExStyle;
    UINT64 ParentWindowHandle;
    WCHAR Caption[256];
} QH_MESSAGE;
