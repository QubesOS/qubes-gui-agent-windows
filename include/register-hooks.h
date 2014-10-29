#pragma once
#include <Windows.h>

// 
typedef struct _HOOK_DATA
{
    BOOL HooksActive;

    HANDLE ServerProcess32; // 32bit hook server
    HANDLE ShutdownEvent32; // event for shutting down 32bit hook server

    // Global hook handles. These are 64-bit (registered directly by gui agent).
    // 32bit hooks stop when their server process terminates.
    HHOOK CbtHook;
    HHOOK CallWndHook;
    HHOOK GetMsgHook;

} HOOK_DATA;

DWORD SetHooks(IN const WCHAR *dllName, OUT HOOK_DATA *hookData);
