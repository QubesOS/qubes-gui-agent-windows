#include <windows.h>
#include <strsafe.h>

#include "main.h"
#include "hook-messages.h"
#include "send.h"
#include "wm.h"

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

// Hook event: window created. Add it to the window list and send notification to gui daemon if it's visible.
static ULONG HookCreateWindow(IN QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    WINDOWINFO wi = { 0 };
    ULONG status;

    LogVerbose("%x", qhm->WindowHandle);

    // sanity check - data from CBT_CREATEWND can have unreliable coordinates
    if (qhm->Width < 0)
        qhm->Width = 0;
    if (qhm->Height)
        qhm->Height = 0;

    wi.cbSize = sizeof(wi);
    wi.dwStyle = qhm->Style;
    wi.dwExStyle = qhm->ExStyle;
    wi.rcWindow.left = qhm->X;
    wi.rcWindow.top = qhm->Y;
    wi.rcWindow.right = qhm->X + qhm->Width;
    wi.rcWindow.bottom = qhm->Y + qhm->Height;

    if (!ShouldAcceptWindow((HWND) qhm->WindowHandle, &wi))
    {
        LogVerbose("ignored");
        return ERROR_SUCCESS; // ignore
    }

    EnterCriticalSection(&g_csWatchedWindows);

    // this sends create/map notifications to gui daemon if the window is visible
    status = AddWindowWithInfo((HWND) qhm->WindowHandle, &wi, NULL);
    if (ERROR_SUCCESS != status)
        perror2(status, "AddWindowWithInfo");

    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

// Hook event: window destroyed. Remove it from the window list and send notification to gui daemon.
static ULONG HookDestroyWindow(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    ULONG status;

    LogVerbose("%x", qhm->WindowHandle);

    EnterCriticalSection(&g_csWatchedWindows);
    status = RemoveWindow(entry);
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
static ULONG HookActivateWindow(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    LogVerbose("0x%x", qhm->WindowHandle);

    if (!entry->IsVisible)
    {
        if (IsWindowVisible(entry->WindowHandle))
        {
            entry->IsVisible = TRUE;
            SendWindowMap(entry);
        }
    }

    return SendWindowHints(entry->WindowHandle, UrgencyHint);
}

// window text changed
static ULONG HookSetWindowText(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    LogVerbose("0x%x '%s'", qhm->WindowHandle, qhm->Caption);

    if (0 == wcscmp(entry->Caption, qhm->Caption))
    {
        // caption not changed
        return ERROR_SUCCESS;
    }

    StringCchCopy(entry->Caption, RTL_NUMBER_OF(entry->Caption), qhm->Caption);

    return SendWindowName(entry->WindowHandle, entry->Caption);
}

// window shown/hidden/moved/resized
static ULONG HookWindowPosChanged(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    BOOL updateNeeded = FALSE;

    LogVerbose("0x%x (%d,%d) %dx%d, flags 0x%x", qhm->WindowHandle, qhm->X, qhm->Y, qhm->Width, qhm->Height, qhm->Flags);

    // TODO: test various flag combinations, I've seen messages with both SWP_HIDEWINDOW and SWP_SHOWWINDOW...
    if ((qhm->Flags & SWP_HIDEWINDOW) && (qhm->Flags & SWP_SHOWWINDOW))
        LogWarning("%x: SWP_HIDEWINDOW | SWP_SHOWWINDOW", entry->WindowHandle);

    // Failsafe: handle minimize here, it's possible that no WM_SIZE is sent for iconic state change...
    if (qhm->X == -32000 && qhm->Y == -32000) // window minimized
    {
        if (entry->IsIconic) // already minimized
        {
            LogVerbose("window 0x%x already iconic", entry->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsIconic = TRUE;

        LogDebug("minimizing 0x%x", entry->WindowHandle);
        return SendWindowFlags(entry->WindowHandle, WINDOW_FLAG_MINIMIZE, 0); // no further processing needed
    }

    if (qhm->Flags & SWP_HIDEWINDOW) // hides the window
    {
        if (!entry->IsVisible) // already hidden
        {
            LogVerbose("window 0x%x already hidden", qhm->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsVisible = FALSE;
        return SendWindowUnmap(entry->WindowHandle);
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

        LogVerbose("window %x position changing (%d,%d) -> (%d,%d)", qhm->WindowHandle, entry->X, entry->Y, qhm->X, qhm->Y);
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

        LogVerbose("window %x size changing (%d,%d) -> (%d,%d)", qhm->WindowHandle, entry->Width, entry->Height, qhm->Width, qhm->Height);
        entry->Width = qhm->Width;
        entry->Height = qhm->Height;
        updateNeeded = TRUE;
    }

    if (updateNeeded)
        return SendWindowConfigure(entry);

    return ERROR_SUCCESS;
}

// window shown/hidden
static ULONG HookShowWindow(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    LogVerbose("0x%x %x", qhm->WindowHandle, qhm->wParam);

    if (qhm->wParam) // shows the window
    {
        if (entry->IsVisible) // already visible
        {
            LogVerbose("window 0x%x already visible", qhm->WindowHandle);
            return ERROR_SUCCESS;
        }

        entry->IsVisible = TRUE;
        return SendWindowMap(entry);
    }

    // window being hidden
    if (!entry->IsVisible) // already hidden
    {
        LogVerbose("window 0x%x already hidden", qhm->WindowHandle);
        return ERROR_SUCCESS;
    }

    entry->IsVisible = FALSE;
    return SendWindowUnmap(entry->WindowHandle);
}

// window minimized/maximized/restored
static ULONG HookSizeWindow(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    BOOL updateNeeded = FALSE;

    LogVerbose("0x%x, flag 0x%x", qhm->WindowHandle, qhm->wParam);

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
static ULONG HookStyleChanged(IN const QH_MESSAGE *qhm, IN WINDOW_DATA *entry)
{
    MODAL_SEARCH_PARAMS modalParams = { 0 };
    ULONG status;

    LogVerbose("0x%x: %s 0x%x", qhm->WindowHandle, qhm->ExStyle ? L"ExStyle" : L"Style", qhm->Style);

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
    WINDOW_DATA *entry = NULL;
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

    LogDebug("%8x: %20S (%4x) %8x %8x\n",
        qhm->WindowHandle,
        qhm->HookId == WH_CBT ? CBTNameFromId(qhm->Message) : MsgNameFromId(qhm->Message),
        qhm->Message,
        qhm->wParam, qhm->lParam);

    entry = FindWindowByHandle((HWND) qhm->WindowHandle);
    if (!entry)
    {
        WINDOWINFO wi = { 0 };
        wi.cbSize = sizeof(wi);
        GetWindowInfo((HWND) qhm->WindowHandle, &wi);
        LogWarning("window 0x%x not tracked, adding", qhm->WindowHandle);
        AddWindowWithInfo((HWND) qhm->WindowHandle, &wi, &entry);
        if (!entry)
            return ERROR_SUCCESS; // ignored
    }

    // Failsafes because apparently window messages aren't as reliable as expected.
    // It's possible that a window is destroyed WITHOUT receiving WM_DESTROY (mostly menus).
    if (!IsWindow(entry->WindowHandle))
    {
        LogDebug("window %x disappeared, removing", entry->WindowHandle);
        return RemoveWindow(entry);
    }

    if (entry->IsVisible && !IsWindowVisible(entry->WindowHandle))
    {
        LogDebug("window %x turned invisible?!", entry->WindowHandle);
        entry->IsVisible = FALSE;
        SendWindowUnmap(entry->WindowHandle);
    }

    if (qhm->HookId != WH_CBT)
    {
        switch (qhm->Message)
        {
        case WM_CREATE:
            status = HookCreateWindow(qhm, entry);
            break;

        case WM_DESTROY:
            status = HookDestroyWindow(qhm, entry);
            break;

        case WM_ACTIVATE:
            status = HookActivateWindow(qhm, entry);
            break;

        case WM_SETTEXT:
            status = HookSetWindowText(qhm, entry);
            break;

        case WM_SHOWWINDOW:
            status = HookShowWindow(qhm, entry);
            break;

        case WM_WINDOWPOSCHANGED:
            status = HookWindowPosChanged(qhm, entry);
            break;

        case WM_SIZE:
            status = HookSizeWindow(qhm, entry);
            break;

        case WM_STYLECHANGED:
            status = HookStyleChanged(qhm, entry);
            break;

        default:
            status = ERROR_SUCCESS;
            break;
        }
    }
    else // CBT messages
    {
        switch (qhm->Message)
        {
        case HCBT_CREATEWND:
            status = HookCreateWindow(qhm, entry);
            break;

        case HCBT_DESTROYWND:
            status = HookDestroyWindow(qhm, entry);
            break;

        case HCBT_ACTIVATE:
            status = HookActivateWindow(qhm, entry);
            break;

        default:
            status = ERROR_SUCCESS;
            break;
        }
    }

    return status;
}
