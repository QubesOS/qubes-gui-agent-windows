#include <windows.h>

#include "main.h"
#include "send.h"
#include "util.h"

#include "log.h"

const WCHAR g_ShellHookClassName[] = L"QubesShellHookClass";
ULONG g_ShellHookMessage = 0;
HWND g_ShellEventsWindow = NULL;

// All code in this file runs in a separate "shell hook" thread.

static void AddWindow(IN HWND window)
{
    WINDOWINFO wi;

    LogDebug("0x%x", window);
    if (window == 0)
        return;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(window, &wi))
    {
        perror("GetWindowInfo");
        return;
    }

    if (!ShouldAcceptWindow(window, &wi))
        return;

    EnterCriticalSection(&g_csWatchedWindows);
    if (!AddWindowWithInfo(window, &wi))
    {
        LogError("AddWindowWithInfo failed");
    }
    LeaveCriticalSection(&g_csWatchedWindows);
}

static void RemoveWindow(IN HWND window)
{
    WATCHED_DC *watchedDC;

    LogDebug("0x%x", window);
    EnterCriticalSection(&g_csWatchedWindows);

    watchedDC = FindWindowByHandle(window);
    if (!watchedDC)
    {
        LeaveCriticalSection(&g_csWatchedWindows);
        return;
    }

    RemoveEntryList(&watchedDC->ListEntry);
    RemoveWindow(watchedDC);
    watchedDC = NULL;

    LeaveCriticalSection(&g_csWatchedWindows);
}

// called from shell hook proc
static ULONG CheckWindowUpdates(IN HWND window)
{
    WINDOWINFO wi;
    WATCHED_DC *watchedDC = NULL;

    LogDebug("0x%x", window);
    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(window, &wi))
        return perror("GetWindowInfo");

    EnterCriticalSection(&g_csWatchedWindows);

    // AddWindowWithInfo() returns an existing pWatchedDC if the window is already on the list.
    watchedDC = AddWindowWithInfo(window, &wi);
    if (!watchedDC)
    {
        LogDebug("AddWindowWithInfo returned NULL");
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    CheckWatchedWindowUpdates(watchedDC, &wi, FALSE, NULL);
    LeaveCriticalSection(&g_csWatchedWindows);
    SendWindowName(window);

    return ERROR_SUCCESS;
}

static LRESULT CALLBACK ShellHookWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
    )
{
    HWND targetWindow = (HWND) lParam;

    if (message == g_ShellHookMessage)
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

    switch (message)
    {
    case WM_CLOSE:
        LogDebug("WM_CLOSE");
        DestroyWindow(window);
        break;
    case WM_DESTROY:
        LogDebug("WM_DESTROY");
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
    return 0;
}

static ULONG CreateShellHookWindow(OUT HWND *window)
{
    WNDCLASSEX windowClass;
    HINSTANCE hInstance = GetModuleHandle(NULL);
    ULONG status;

    LogVerbose("start");

    if (!window)
        return ERROR_INVALID_PARAMETER;

    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.lpfnWndProc = ShellHookWindowProc;
    windowClass.hInstance = hInstance;
    windowClass.lpszClassName = g_ShellHookClassName;

    if (!RegisterClassEx(&windowClass))
        return perror("RegisterClassEx");

    *window = CreateWindow(g_ShellHookClassName, L"QubesShellHook",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInstance, NULL);

    if (*window == NULL)
        return perror("CreateWindow");

    LogDebug("shell hook window: 0x%x", *window);

    ShowWindow(*window, SW_HIDE);
    UpdateWindow(*window);

    if (!RegisterShellHookWindow(*window))
    {
        status = perror("RegisterShellHookWindow");
        DestroyWindow(*window);
        UnregisterClass(g_ShellHookClassName, hInstance);
        return status;
    }

    if (!g_ShellHookMessage)
        g_ShellHookMessage = RegisterWindowMessage(L"SHELLHOOK");

    if (!g_ShellHookMessage)
        return perror("RegisterWindowMessage");

    return ERROR_SUCCESS;
}

static ULONG ShellHookMessageLoop(void)
{
    MSG message;
    HINSTANCE instance = GetModuleHandle(NULL);

    LogDebug("start");
    while (GetMessage(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    LogDebug("exiting");
    return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(void *param)
{
    ULONG status;

    LogDebug("start");
    if (ERROR_SUCCESS != AttachToInputDesktop())
        return perror("AttachToInputDesktop");

    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWindow))
        return perror("CreateShellHookWindow");

    InvalidateRect(NULL, NULL, TRUE); // repaint everything
    if (ERROR_SUCCESS != (status = ShellHookMessageLoop()))
        return status;

    if (!UnregisterClass(g_ShellHookClassName, NULL))
        return perror("UnregisterClass");

    return ERROR_SUCCESS;
}
