#include <windows.h>

#ifdef _AMD64_
#include "log.h"
#endif

DWORD SetHooks(WCHAR *dllName)
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

    if (!SetWindowsHookEx(WH_CBT, (HOOKPROC)hookProc, hookDll, 0))
        return perror("SetWindowsHookEx(CBTProc)");

    // CallWndProc hook
    hookProc = GetProcAddress(hookDll, "CallWndProc");
    if (!hookProc)
        return perror("GetProcAddress(CallWndProc)");

    if (!SetWindowsHookEx(WH_CALLWNDPROC, (HOOKPROC)hookProc, hookDll, 0))
        return perror("SetWindowsHookEx(CallWndProc)");

    // GetMsgProc hook
    hookProc = GetProcAddress(hookDll, "GetMsgProc");
    if (!hookProc)
        return perror("GetProcAddress(GetMsgProc)");

    if (!SetWindowsHookEx(WH_GETMESSAGE, (HOOKPROC)hookProc, hookDll, 0))
        return perror("SetWindowsHookEx(GetMsgProc)");

    return ERROR_SUCCESS;
}
