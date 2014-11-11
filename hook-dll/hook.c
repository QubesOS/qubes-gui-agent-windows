#include <windows.h>
#include <strsafe.h>

#include "hook.h"
#include "hook-messages.h"

HANDLE g_Slot = NULL; // IPC mailslot

BOOL APIENTRY DllMain(HMODULE module, DWORD reasonForCall, void *reserved)
{
    switch (reasonForCall)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        OutputDebugString(L"QHook: +ATTACH\n");
        if (!g_Slot)
            g_Slot = CreateFile(HOOK_IPC_NAME, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        break;

    case DLL_PROCESS_DETACH:
        if (g_Slot)
            CloseHandle(g_Slot);
        OutputDebugString(L"QHook: -DETACH\n");
        break;
    }
    return TRUE;
}

static void SendMsg(IN OUT QH_MESSAGE *qhm)
{
    DWORD written;

#ifdef _AMD64_
    qhm->Is64bit = TRUE;
#else
    qhm->Is64bit = FALSE;
#endif
    if (g_Slot)
        WriteFile(g_Slot, qhm, sizeof(QH_MESSAGE), &written, NULL);
}

void ProcessMessage(IN OUT QH_MESSAGE *qhm)
{
    WINDOWPOS *wp = (WINDOWPOS *) qhm->lParam;
    STYLESTRUCT *ss = (STYLESTRUCT *) qhm->lParam;
    CREATESTRUCT *cs = (CREATESTRUCT *) qhm->lParam;

    switch (qhm->Message)
    {
    case WM_CREATE: // window created (but not yet visible)
        qhm->X = cs->x;
        qhm->Y = cs->y;
        qhm->Width = cs->cx;
        qhm->Height = cs->cy;
        qhm->Style = cs->style;
        qhm->ExStyle = cs->dwExStyle;
        qhm->ParentWindowHandle = (UINT64) cs->hwndParent;
        if (cs->lpszName)
        {
            StringCbCopy(qhm->Caption, sizeof(qhm->Caption), cs->lpszName);
        }
        SendMsg(qhm);
        break;

    case WM_DESTROY:
        SendMsg(qhm);
        break;

    // WM_MOVE and WM_SIZE only have coordinatef of client area... useless for us

    case WM_ACTIVATE:
        // wParam is WA_ACTIVE/WA_CLICKACTIVE/WA_INACTIVE
        // If the low-order word of wParam is WA_INACTIVE, lParam is the handle to the window being activated.
        // If the low-order word of wParam is WA_ACTIVE or WA_CLICKACTIVE, lParam is the handle to the window being deactivated.
        // This handle can be NULL.
        SendMsg(qhm);
        break;

    case WM_SETTEXT:
        if (qhm->lParam)
        {
            StringCbCopy(qhm->Caption, sizeof(qhm->Caption), (WCHAR *) qhm->lParam);
            SendMsg(qhm);
        }
        break;

    case WM_PAINT:
    case WM_NCPAINT:
        SendMsg(qhm);
        break;

    case WM_SHOWWINDOW:
        // wParam is show/hide
        SendMsg(qhm);
        break;

    //case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
        qhm->Flags = wp->flags;
        qhm->X = wp->x;
        qhm->Y = wp->y;
        qhm->Width = wp->cx;
        qhm->Height = wp->cy;
        SendMsg(qhm);
        break;

    case WM_STYLECHANGED:
        // gui agent's handler will still need to check wParam...
        if (qhm->wParam == GWL_STYLE)
            qhm->Style = ss->styleNew;
        else if (qhm->wParam == GWL_EXSTYLE)
            qhm->ExStyle = ss->styleNew;
        else
        {
            OutputDebugString(L"QHook: WM_STYLECHANGED unknown wParam");
            break;
        }
        SendMsg(qhm);
        break;
        /* debug only - capture ALL messages
    default:
        SendMsg(qhm);
        break;
        */
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
        OutputDebugString(L"[!] CWP: <0\n");
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

LRESULT CALLBACK CallWndRetProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
    )
{
    CWPRETSTRUCT *cwpr = (CWPRETSTRUCT *) lParam;
    QH_MESSAGE qhm = { 0 };

    if (code < 0)
    {
        OutputDebugString(L"[!] CWPR: <0\n");
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    qhm.HookId = WH_CALLWNDPROCRET;
    qhm.Message = cwpr->message;
    qhm.WindowHandle = (UINT64) cwpr->hwnd;
    qhm.wParam = cwpr->wParam;
    qhm.lParam = cwpr->lParam;

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
        OutputDebugString(L"[!] GMP: <0\n");
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
        OutputDebugString(L"[!] CBTP: <0\n");
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
