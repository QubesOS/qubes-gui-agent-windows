#include <windows.h>
#include <strsafe.h>

#include "main.h"
#include "hook-messages.h"
#include "send.h"

#include "log.h"

// Enumerate top-level windows, searching for one that is modal
// in relation to a parent one (passed in lParam).
static BOOL WINAPI FindModalChildProc(IN HWND hwnd, IN LPARAM lParam)
{
    MODAL_SEARCH_PARAMS *msp = (MODAL_SEARCH_PARAMS *) lParam;
    LONG wantedStyle = WS_POPUP | WS_VISIBLE;
    HWND owner = GetWindow(hwnd, GW_OWNER);

    // Modal windows are not child windows but owned windows.
    if (owner != msp->ParentWindow)
        return TRUE;

    if ((GetWindowLong(hwnd, GWL_STYLE) & wantedStyle) != wantedStyle)
        return TRUE;

    msp->ModalWindow = hwnd;
    LogVerbose("0x%x: seems OK for 0x%x", hwnd, msp->ParentWindow);
    return FALSE; // stop enumeration
}

// Hook event: window created. Add it to the window list and send notification to gui daemon.
static ULONG HookCreateWindow(IN const QH_MESSAGE *qhm)
{
    WINDOWINFO wi = { 0 };
    ULONG status;

    LogVerbose("%x", qhm->WindowHandle);

    wi.cbSize = sizeof(wi);
    wi.dwStyle = qhm->Style;
    wi.dwExStyle = qhm->ExStyle;
    wi.rcWindow.left = qhm->X;
    wi.rcWindow.top = qhm->Y;
    wi.rcWindow.right = qhm->X + qhm->Width;
    wi.rcWindow.bottom = qhm->Y + qhm->Height;

    if (!ShouldAcceptWindow((HWND) qhm->WindowHandle, &wi))
        return ERROR_SUCCESS; // ignore

    EnterCriticalSection(&g_csWatchedWindows);

    // this sends notifications about current window state to gui daemon
    status = AddWindowWithInfo((HWND) qhm->WindowHandle, &wi, NULL);
    if (ERROR_SUCCESS != status)
        perror2(status, "AddWindowWithInfo");

    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

// Hook event: window destroyed. Remove it from the window list and send notification to gui daemon.
static ULONG HookDestroyWindow(IN const QH_MESSAGE *qhm)
{
    WINDOW_DATA *windowEntry;
    ULONG status;

    LogVerbose("%x", qhm->WindowHandle);

    EnterCriticalSection(&g_csWatchedWindows);
    windowEntry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (windowEntry)
    {
        status = RemoveWindow(windowEntry);
    }
    else
    {
        LogWarning("window %x not tracked", qhm->WindowHandle);
        // in theory we should be tracking all windows
        status = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

/*
FIXME: only send activation notifications if activation
was not a result of user action (HandleFocus).
This would need keeping track of window state changes in
WATCHED_DC (which will probably be needed anyway).

Anyway, gui daemon can't actually properly handle activation
requests (and they can be seen as a security risk -- focus
stealing). Ideally gui daemon should set the window's z-order
so that it's on top of all other windows from the same domain,
but not above current top-level window if its domain is different.
*/
static ULONG HookActivateWindow(IN const QH_MESSAGE *qhm)
{
    LogVerbose("%x", qhm->WindowHandle);

    return SendWindowHints((HWND) qhm->WindowHandle, UrgencyHint);
}

// window text changed
static ULONG HookSetWindowText(IN const QH_MESSAGE *qhm)
{
    WINDOW_DATA *entry;

    LogVerbose("0x%x '%s'", qhm->WindowHandle, qhm->Caption);

    entry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (!entry)
    {
        LogWarning("window 0x%x not tracked", qhm->WindowHandle);
        return ERROR_SUCCESS;
    }

    if (0 == wcscmp(entry->Caption, qhm->Caption))
    {
        // caption not changed
        return ERROR_SUCCESS;
    }

    StringCchCopy(entry->Caption, RTL_NUMBER_OF(entry->Caption), qhm->Caption);

    return SendWindowName((HWND) qhm->WindowHandle, entry->Caption);
}

// window shown/hidden/moved/resized
static ULONG HookShowWindow(IN const QH_MESSAGE *qhm)
{
    WINDOW_DATA *entry;
    BOOL updateNeeded = FALSE;

    LogVerbose("0x%x (%d,%d) %dx%d, flags 0x%x", qhm->WindowHandle, qhm->X, qhm->Y, qhm->Width, qhm->Height, qhm->Flags);

    entry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (!entry)
    {
        LogWarning("window 0x%x not tracked", qhm->WindowHandle);
        return ERROR_SUCCESS;
    }

    // TODO: test various flag combinations, I've seen messages with both SWP_HIDEWINDOW and SWP_SHOWWINDOW...
    if (qhm->Flags & SWP_HIDEWINDOW) // hides the window
    {
        if (!entry->IsVisible) // already hidden
        {
            LogVerbose("window 0x%x already hidden", qhm->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsVisible = FALSE;
        return SendWindowUnmap((HWND) qhm->WindowHandle);
    }

    if (qhm->Flags & SWP_SHOWWINDOW) // shows the window
    {
        if (entry->IsVisible) // already visible
        {
            LogVerbose("window 0x%x already visible", qhm->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsVisible = TRUE;
        return SendWindowMap(entry);
    }

    if (!(qhm->Flags & SWP_NOMOVE)) // window moved
    {
        if (qhm->X == entry->X && qhm->Y == entry->Y) // same position
        {
            LogVerbose("window 0x%x position not changed (%d,%d)", qhm->WindowHandle, qhm->X, qhm->Y);
            return ERROR_SUCCESS;
        }

        entry->X = qhm->X;
        entry->Y = qhm->Y;
        updateNeeded = TRUE;
    }

    if (!(qhm->Flags & SWP_NOSIZE)) // window resized
    {
        if (qhm->Width == entry->Width && qhm->Height == entry->Height) // same size
        {
            LogVerbose("window 0x%x size not changed (%dx%d)", qhm->WindowHandle, qhm->Width, qhm->Height);
            return ERROR_SUCCESS;
        }

        entry->Width = qhm->Width;
        entry->Height = qhm->Height;
        updateNeeded = TRUE;
    }

    if (updateNeeded)
        return SendWindowConfigure(entry);

    return ERROR_SUCCESS;
}

// window minimized/maximized/restored
static ULONG HookSizeWindow(IN const QH_MESSAGE *qhm)
{
    WINDOW_DATA *entry;
    BOOL updateNeeded = FALSE;

    LogVerbose("0x%x, flag 0x%x", qhm->WindowHandle, qhm->wParam);

    entry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (!entry)
    {
        LogWarning("window 0x%x not tracked", qhm->WindowHandle);
        return ERROR_SUCCESS;
    }

    // we only care about minimized state here, resizing is handled by WM_SHOWWINDOW
    switch (qhm->wParam)
    {
    case SIZE_MINIMIZED:
        if (entry->IsIconic) // already minimized
        {
            LogVerbose("window 0x%x already iconic", entry->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsIconic = TRUE;

        LogDebug("minimizing 0x%x", entry->WindowHandle);
        return SendWindowFlags(entry->WindowHandle, WINDOW_FLAG_MINIMIZE, 0);

    case SIZE_MAXIMIZED:
    case SIZE_RESTORED:
        if (!entry->IsIconic) // already not minimized
        {
            LogVerbose("window 0x%x not iconic", qhm->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsIconic = FALSE;

        LogDebug("restoring 0x%x", entry->WindowHandle);
        return SendWindowFlags(entry->WindowHandle, 0, WINDOW_FLAG_MINIMIZE);
    }

    return ERROR_SUCCESS;
}

// window style changed
static ULONG HookStyleChanged(IN const QH_MESSAGE *qhm)
{
    WINDOW_DATA *entry;
    MODAL_SEARCH_PARAMS modalParams = { 0 };
    ULONG status;

    LogVerbose("0x%x: %s 0x%x", qhm->WindowHandle, qhm->ExStyle ? L"ExStyle" : L"Style", qhm->Style);

    entry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (!entry)
    {
        LogWarning("window 0x%x not tracked", qhm->WindowHandle);
        return ERROR_SUCCESS;
    }

    if (!entry->IsVisible)
        return ERROR_SUCCESS;

    if (!qhm->ExStyle && (qhm->Style & WS_DISABLED))
    {
        // possibly showing a modal window
        LogDebug("0x%x is WS_DISABLED, searching for modal window", entry->WindowHandle);
        modalParams.ParentWindow = entry->WindowHandle;
        modalParams.ModalWindow = NULL;

        if (!EnumWindows(FindModalChildProc, (LPARAM) &modalParams))
            return perror("EnumWindows");

        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            WINDOW_DATA *modalWindow = FindWindowByHandle(modalParams.ModalWindow);
            if (modalWindow && !modalWindow->ModalParent)
            {
                // need to toggle map since this is the only way to change modal status for gui daemon
                modalWindow->ModalParent = entry->WindowHandle;
                status = SendWindowUnmap(modalWindow->WindowHandle);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowUnmap");

                status = SendWindowMap(modalWindow);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowMap");
            }
        }
    }

    // FIXME: handle the opposite case?
    // In theory it shouldn't happen, window is modal until it's destroyed...

    return ERROR_SUCCESS;
}

// Process events from hooks.
// Updates watched windows state.
ULONG HandleHookEvent(IN HANDLE hookIpc, IN OUT OVERLAPPED *hookAsyncState, IN QH_MESSAGE *qhm)
{
    DWORD cbRead;
    ULONG status;

    if (!GetOverlappedResult(hookIpc, hookAsyncState, &cbRead, FALSE))
    {
        return perror("GetOverlappedResult(hook mailslot)");
    }

    if (cbRead != sizeof(*qhm))
    {
        LogWarning("Invalid hook message size: %d (expected %d)", cbRead, sizeof(*qhm));
        // non-fatal although shouldn't happen
        return ERROR_SUCCESS;
    }

    LogDebug("%8x: %4x %8x %8x\n",
        qhm->WindowHandle,
        qhm->Message,
        //qhm.HookId == WH_CBT ? CBTNameFromId(qhm.Message) : MsgNameFromId(qhm.Message),
        qhm->wParam, qhm->lParam);

    switch (qhm->Message)
    {
    case WM_CREATE:
        status = HookCreateWindow(qhm);
        break;

    case WM_DESTROY:
        status = HookDestroyWindow(qhm);
        break;

    case WM_ACTIVATE:
        status = HookActivateWindow(qhm);
        break;

    case WM_SETTEXT:
        status = HookSetWindowText(qhm);
        break;

    case WM_SHOWWINDOW:
        status = HookShowWindow(qhm);
        break;

    case WM_SIZE:
        status = HookSizeWindow(qhm);
        break;

    case WM_STYLECHANGED:
        status = HookStyleChanged(qhm);
        break;

    default:
        status = ERROR_SUCCESS;
        break;
    }

    return status;
}
