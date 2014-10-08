#include <windows.h>

#include "hook.h"
#include "hook-messages.h"

HANDLE g_Slot = INVALID_HANDLE_VALUE;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, void *lpReserved)
{
    WCHAR buf[128] = { 0 };

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        OutputDebugString(L"QHook: +ATTACH");
        g_Slot = CreateFile(HOOK_IPC_NAME, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        break;

    case DLL_PROCESS_DETACH:
        if (g_Slot != INVALID_HANDLE_VALUE)
            CloseHandle(g_Slot);
        OutputDebugString(L"QHook: -DETACH");
        break;
    }
    return TRUE;
}

static void SendMsg(QH_MESSAGE *msg)
{
    DWORD written;
    if (g_Slot != INVALID_HANDLE_VALUE)
        WriteFile(g_Slot, msg, sizeof(QH_MESSAGE), &written, NULL);
}

void ProcessMessage(QH_MESSAGE *qhm)
{
    WINDOWPOS *wp = (WINDOWPOS*) qhm->lParam;
    STYLESTRUCT *ss = (STYLESTRUCT*) qhm->lParam;

    switch (qhm->Message)
    {
    case WM_ACTIVATE:
        SendMsg(qhm);
        break;

    case WM_WINDOWPOSCHANGED:
        qhm->Flags = wp->flags;
        qhm->X = wp->x;
        qhm->Y = wp->y;
        qhm->Width = wp->cx;
        qhm->Height = wp->cy;
        SendMsg(qhm);
        break;

    case WM_STYLECHANGED:
        qhm->Style = ss->styleNew;
        qhm->StyleOld = ss->styleOld;
        SendMsg(qhm);
        break;
    }
}

LRESULT CALLBACK CallWndProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
    )
{
    CWPSTRUCT *cwp = (CWPSTRUCT *) lParam;
    QH_MESSAGE qhm = { 0 };

    if (code < 0)
    {
        OutputDebugString(L"[!] CWP: <0");
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    qhm.HookId = WH_CALLWNDPROC;
    qhm.Message = cwp->message;
    qhm.WindowHandle = (UINT64) cwp->hwnd;
    qhm.wParam = cwp->wParam;
    qhm.lParam = cwp->lParam;

    ProcessMessage(&qhm);

    return CallNextHookEx(NULL, code, wParam, lParam);
}

LRESULT CALLBACK GetMsgProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
    )
{
    MSG *msg = (MSG*) lParam;
    QH_MESSAGE qhm = { 0 };

    if (code < 0)
    {
        OutputDebugString(L"[!] GMP: <0");
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    qhm.HookId = WH_GETMESSAGE;
    qhm.Message = msg->message;
    qhm.WindowHandle = (UINT64) msg->hwnd;
    qhm.wParam = msg->wParam;
    qhm.lParam = msg->lParam;

    ProcessMessage(&qhm);

    return CallNextHookEx(NULL, code, wParam, lParam);
}

LRESULT CALLBACK CBTProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
    )
{
    MSG *msg = (MSG *) lParam;
    QH_MESSAGE qhm = { 0 };

    if (code < 0)
    {
        OutputDebugString(L"[!] CBTP: <0");
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    qhm.HookId = WH_CBT;
    qhm.WindowHandle = (UINT64) wParam;
    qhm.lParam = lParam;

    switch (code)
    {
    case HCBT_ACTIVATE:
        qhm.Message = HCBT_ACTIVATE;
        break;

    case HCBT_CREATEWND:
        qhm.Message = HCBT_CREATEWND;
        break;

    case HCBT_DESTROYWND:
        qhm.Message = HCBT_DESTROYWND;
        break;

    case HCBT_MINMAX:
        qhm.Message = HCBT_MINMAX;
        qhm.Flags = LOWORD(lParam);
        break;

    case HCBT_MOVESIZE:
        qhm.Message = HCBT_MOVESIZE;
        qhm.X = ((RECT*) lParam)->left;
        qhm.Y = ((RECT*) lParam)->top;
        qhm.Width = ((RECT*) lParam)->right - ((RECT*) lParam)->left;
        qhm.Height = ((RECT*) lParam)->bottom - ((RECT*) lParam)->top;
        break;

    case HCBT_SETFOCUS:
        qhm.Message = HCBT_SETFOCUS;
        break;

    case HCBT_SYSCOMMAND:
        qhm.Message = HCBT_SYSCOMMAND;
        break;
    }

    SendMsg(&qhm);

    return 0; // allow the action
}
