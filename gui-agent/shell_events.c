#include <windows.h>

#include "main.h"
#include "send.h"
#include "util.h"

#include "log.h"

const WCHAR g_szClassName[] = L"QubesShellHookClass";
ULONG g_uShellHookMessage = 0;
HWND g_ShellEventsWindow = NULL;

// All code in this file runs in a separate "shell hook" thread.

void AddWindow(HWND hWnd)
{
    WINDOWINFO wi;

    LogDebug("0x%x", hWnd);
    if (hWnd == 0)
        return;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
    {
        perror("GetWindowInfo");
        return;
    }

    if (!ShouldAcceptWindow(hWnd, &wi))
        return;

    EnterCriticalSection(&g_csWatchedWindows);

    AddWindowWithInfo(hWnd, &wi);

    LeaveCriticalSection(&g_csWatchedWindows);
}

void RemoveWindow(HWND hWnd)
{
    PWATCHED_DC pWatchedDC;

    LogDebug("0x%x", hWnd);
    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (!pWatchedDC)
    {
        LeaveCriticalSection(&g_csWatchedWindows);
        return;
    }

    RemoveEntryList(&pWatchedDC->le);
    RemoveWatchedDC(pWatchedDC);
    pWatchedDC = NULL;

    LeaveCriticalSection(&g_csWatchedWindows);
}

// called from shell hook proc
ULONG CheckWindowUpdates(HWND hWnd)
{
    WINDOWINFO wi;
    PWATCHED_DC pWatchedDC = NULL;

    LogDebug("0x%x", hWnd);
    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return perror("GetWindowInfo");

    EnterCriticalSection(&g_csWatchedWindows);

    // AddWindowWithInfo() returns an existing pWatchedDC if the window is already on the list.
    pWatchedDC = AddWindowWithInfo(hWnd, &wi);
    if (!pWatchedDC)
    {
        LogDebug("AddWindowWithInfo returned NULL");
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    CheckWatchedWindowUpdates(pWatchedDC, &wi, FALSE, NULL);

    LeaveCriticalSection(&g_csWatchedWindows);

    send_wmname(hWnd);

    return ERROR_SUCCESS;
}

LRESULT CALLBACK ShellHookWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam
    )
{
    HWND targetWindow = (HWND) lParam;

    if (uMsg == g_uShellHookMessage)
    {
        switch (wParam)
        {
        case HSHELL_WINDOWCREATED:
            AddWindow(targetWindow);
            break;

        case HSHELL_WINDOWDESTROYED:
            RemoveWindow(targetWindow);
            break;

        case HSHELL_REDRAW:
            LogDebug("HSHELL_REDRAW");
            goto update;
        case HSHELL_RUDEAPPACTIVATED:
            LogDebug("HSHELL_RUDEAPPACTIVATED");
            goto update;
        case HSHELL_WINDOWACTIVATED:
            LogDebug("HSHELL_RUDEAPPACTIVATED");
            goto update;
        case HSHELL_GETMINRECT:
            LogDebug("HSHELL_GETMINRECT");
            targetWindow = ((SHELLHOOKINFO*) lParam)->hwnd;
        update:
            CheckWindowUpdates(targetWindow);
            break;
            /*
            case HSHELL_WINDOWREPLACING:
            case HSHELL_WINDOWREPLACED:
            case HSHELL_FLASH:
            case HSHELL_ENDTASK:
            case HSHELL_APPCOMMAND:
            break;
            */
        }

        return 0;
    }

    switch (uMsg)
    {
    case WM_WTSSESSION_CHANGE:
        LogInfo("session change: event 0x%x, session %d", wParam, lParam);
        //if (!CreateThread(0, 0, ResetWatch, NULL, 0, NULL))
        //    perror("CreateThread(ResetWatch)");
        break;
    case WM_CLOSE:
        LogDebug("WM_CLOSE");
        //if (!WTSUnRegisterSessionNotification(hwnd))
        //    perror("WTSUnRegisterSessionNotification");
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        LogDebug("WM_DESTROY");
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

ULONG CreateShellHookWindow(HWND *pWindow)
{
    WNDCLASSEX wc;
    HWND hwnd;
    HINSTANCE hInstance = GetModuleHandle(NULL);
    ULONG uResult;

    LogVerbose("start");

    if (!pWindow)
        return ERROR_INVALID_PARAMETER;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = ShellHookWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = g_szClassName;

    if (!RegisterClassEx(&wc))
        return perror("RegisterClassEx");

    hwnd = CreateWindow(g_szClassName, L"QubesShellHook",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInstance, NULL);

    if (hwnd == NULL)
        return perror("CreateWindow");
    LogDebug("shell hook window: 0x%x", hwnd);

    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    if (!RegisterShellHookWindow(hwnd))
    {
        uResult = perror("RegisterShellHookWindow");
        DestroyWindow(hwnd);
        UnregisterClass(g_szClassName, hInstance);
        return uResult;
    }
    /*
    if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_ALL_SESSIONS))
    {
    uResult = perror("WTSRegisterSessionNotification");
    DestroyWindow(hwnd);
    UnregisterClass(g_szClassName, hInstance);
    return uResult;
    }
    */
    if (!g_uShellHookMessage)
        g_uShellHookMessage = RegisterWindowMessage(L"SHELLHOOK");

    if (!g_uShellHookMessage)
        return perror("RegisterWindowMessage");

    *pWindow = hwnd;

    return ERROR_SUCCESS;
}

ULONG ShellHookMessageLoop()
{
    MSG Msg;
    HINSTANCE hInstance = GetModuleHandle(NULL);

    LogDebug("start");
    while (GetMessage(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    LogDebug("exiting");
    return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(PVOID pParam)
{
    ULONG uResult;

    LogDebug("start");
    if (ERROR_SUCCESS != AttachToInputDesktop())
        return perror("AttachToInputDesktop");

    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWindow))
        return perror("CreateShellHookWindow");

    InvalidateRect(NULL, NULL, TRUE); // repaint everything
    if (ERROR_SUCCESS != (uResult = ShellHookMessageLoop()))
        return uResult;

    if (!UnregisterClass(g_szClassName, NULL))
        return perror("UnregisterClass");

    return ERROR_SUCCESS;
}
