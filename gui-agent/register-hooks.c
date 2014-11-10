#include <windows.h>
#include "register-hooks.h"

// This can be used by either 64bit or 32bit code.
// 32bit hook server includes this code.

#ifdef _AMD64_
#include "log.h"
#endif

DWORD SetHooks(IN const WCHAR *dllName, OUT HOOK_DATA *hookData)
{
    HMODULE hookDll = NULL;
    void *hookProc = NULL;

    hookDll = LoadLibrary(dllName);
    if (!hookDll)
        return perror("LoadLibrary");

    // CBT hook
    hookProc = GetProcAddress(hookDll, "CBTProc");
    if (!hookProc)
        return perror("GetProcAddress(CBTProc)");

    hookData->CbtHook = SetWindowsHookEx(WH_CBT, (HOOKPROC) hookProc, hookDll, 0);
    if (!hookData->CbtHook)
        return perror("SetWindowsHookEx(CBTProc)");

    // CallWndProc hook
    hookProc = GetProcAddress(hookDll, "CallWndProc");
    if (!hookProc)
        return perror("GetProcAddress(CallWndProc)");

    hookData->CallWndHook = SetWindowsHookEx(WH_CALLWNDPROC, (HOOKPROC) hookProc, hookDll, 0);
    if (!hookData->CallWndHook)
        return perror("SetWindowsHookEx(CallWndProc)");

    // CallWndProcRet hook
    hookProc = GetProcAddress(hookDll, "CallWndRetProc");
    if (!hookProc)
        return perror("GetProcAddress(CallWndRetProc)");

    hookData->CallWndRetHook = SetWindowsHookEx(WH_CALLWNDPROCRET, (HOOKPROC) hookProc, hookDll, 0);
    if (!hookData->CallWndRetHook)
        return perror("SetWindowsHookEx(CallWndRetProc)");

    // GetMsgProc hook
    hookProc = GetProcAddress(hookDll, "GetMsgProc");
    if (!hookProc)
        return perror("GetProcAddress(GetMsgProc)");

    hookData->GetMsgHook = SetWindowsHookEx(WH_GETMESSAGE, (HOOKPROC) hookProc, hookDll, 0);
    if (!hookData->GetMsgHook)
        return perror("SetWindowsHookEx(GetMsgProc)");

    return ERROR_SUCCESS;
}
