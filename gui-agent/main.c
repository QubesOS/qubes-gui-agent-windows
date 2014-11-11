#include <windows.h>
#include <winsock2.h>
#include <stdlib.h>

#include <xenstore.h>

#include "main.h"
#include "vchan.h"
#include "qvcontrol.h"
#include "resolution.h"
#include "shell_events.h"
#include "send.h"
#include "handlers.h"
#include "util.h"
#include "hook-messages.h"
#include "register-hooks.h"

// windows-utils
#include "log.h"
#include "config.h"

#include <strsafe.h>

#define FULLSCREEN_ON_EVENT_NAME L"WGA_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"WGA_FULLSCREEN_OFF"

// If set, only invalidate parts of the screen that changed according to
// qvideo's dirty page scan of surface memory buffer.
BOOL g_UseDirtyBits;

LONG g_ScreenHeight;
LONG g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;
BOOL g_SeamlessMode = TRUE;

HOOK_DATA g_HookData = { 0 };

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

char g_DomainName[256] = "<unknown>";

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

HWND g_DesktopWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

static ULONG ProcessUpdatedWindows(IN BOOL updateEverything, IN HDC screenDC);

// watched windows critical section must be entered
// Returns ERROR_SUCCESS if the window was added OR ignored (windowEntry is NULl if ignored).
// Other errors mean fatal conditions.
ULONG AddWindowWithInfo(IN HWND window, IN const WINDOWINFO *windowInfo, OUT WATCHED_DC **windowEntry)
{
    WATCHED_DC *entry = NULL;
    ULONG status;

    if (!windowInfo || !windowEntry)
        return ERROR_INVALID_PARAMETER;

    LogDebug("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x",
        window, windowInfo->rcWindow.left, windowInfo->rcWindow.top, windowInfo->rcWindow.right, windowInfo->rcWindow.bottom,
        windowInfo->dwStyle, windowInfo->dwExStyle);

    entry = FindWindowByHandle(window);
    if (entry) // already in list
    {
        *windowEntry = entry;
        return ERROR_SUCCESS;
    }

    // empty window rectangle? ignore
    if ((windowInfo->rcWindow.top - windowInfo->rcWindow.bottom == 0) || (windowInfo->rcWindow.right - windowInfo->rcWindow.left == 0))
    {
        LogDebug("window rectangle is empty");
        *windowEntry = NULL;
        return ERROR_SUCCESS;
    }

    entry = (WATCHED_DC *) malloc(sizeof(WATCHED_DC));
    if (!entry)
    {
        LogError("Failed to malloc WATCHED_DC");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    ZeroMemory(entry, sizeof(WATCHED_DC));

    entry->IsVisible = IsWindowVisible(window);
    entry->IsIconic = IsIconic(window);

    entry->TimeModalChecked = GetTickCount();

    LogDebug("0x%x: visible=%d, iconic=%d", entry->WindowHandle, entry->IsVisible, entry->IsIconic);

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (windowInfo->rcWindow.right - windowInfo->rcWindow.left == g_ScreenWidth
        && windowInfo->rcWindow.bottom - windowInfo->rcWindow.top == g_ScreenHeight)
    {
        LogDebug("popup too large: %dx%d, screen %dx%d",
            windowInfo->rcWindow.right - windowInfo->rcWindow.left,
            windowInfo->rcWindow.bottom - windowInfo->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
        entry->IsOverrideRedirect = FALSE;
    }
    else
    {
        // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
        if ((windowInfo->dwStyle & WS_CAPTION) == WS_CAPTION)
        {
            // normal window
            entry->IsOverrideRedirect = FALSE;
        }
        else if (((windowInfo->dwStyle & WS_SYSMENU) == WS_SYSMENU) && ((windowInfo->dwExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
        {
            // Metro apps without WS_CAPTION.
            // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
            // but I guess MS doesn't adhere to its own standards...
            entry->IsOverrideRedirect = FALSE;
        }
        else
            entry->IsOverrideRedirect = TRUE;
    }

    if (entry->IsOverrideRedirect)
    {
        LogDebug("popup: %dx%d, screen %dx%d",
            windowInfo->rcWindow.right - windowInfo->rcWindow.left,
            windowInfo->rcWindow.bottom - windowInfo->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
    }

    entry->WindowHandle = window;
    entry->WindowRect = windowInfo->rcWindow;

    entry->PfnArray = (PFN_ARRAY *) malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
    if (!entry->PfnArray)
    {
        LogError("Failed to malloc PFN array");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    entry->MaxWidth = g_ScreenWidth;
    entry->MaxHeight = g_ScreenHeight;

    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // send window info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry); // also maps (shows) the window if it's visible and not minimized
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowCreate");

        status = SendWindowName(window, NULL);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowName");
    }

    *windowEntry = entry;
    return ERROR_SUCCESS;
}

ULONG RemoveWindow(IN OUT WATCHED_DC *watchedDC)
{
    ULONG status;

    if (!watchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("window 0x%x", watchedDC->WindowHandle);
    free(watchedDC->PfnArray);

    if (g_VchanClientConnected)
    {
        status = SendWindowUnmap(watchedDC->WindowHandle);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowUnmap");

        if (watchedDC->WindowHandle) // never destroy screen "window"
        {
            status = SendWindowDestroy(watchedDC->WindowHandle);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowDestroy");
        }
    }

    free(watchedDC);

    return ERROR_SUCCESS;
}

static ULONG StartHooks(IN OUT HOOK_DATA *hookData)
{
    ULONG status;

    LogVerbose("start");

    // Event for shutting down 32bit hooks.
    if (!hookData->ShutdownEvent32)
    {
        hookData->ShutdownEvent32 = CreateNamedEvent(HOOK32_SHUTDOWN_EVENT_NAME);
        if (!hookData->ShutdownEvent32)
            return ERROR_UNIDENTIFIED_ERROR;
    }

    // Start 64-bit hooks.
    status = SetHooks(HOOK_DLL_NAME_64, hookData);
    if (ERROR_SUCCESS != status)
        return perror2(status, "SetHooks");

    // Start the 32-bit hook server. It exits when the hookShutdownEvent is signaled.
    status = StartProcess(HOOK_SERVER_NAME_32, &hookData->ServerProcess32);
    if (ERROR_SUCCESS != status)
        return perror2(status, "StartProcess");

    hookData->HooksActive = TRUE;

    return ERROR_SUCCESS;
}

static ULONG StopHooks(IN OUT HOOK_DATA *hookData)
{
    LogVerbose("start");

    if (!hookData->HooksActive)
        return ERROR_SUCCESS; // nothing to do

    // Shutdown QGuiHookServer32.
    if (!SetEvent(hookData->ShutdownEvent32))
        return perror("SetEvent");

    if (WAIT_OBJECT_0 != WaitForSingleObject(hookData->ShutdownEvent32, 1000))
    {
        LogWarning("32bit hook server didn't exit in time, killing it");
        TerminateProcess(hookData->ServerProcess32, 0);
        CloseHandle(hookData->ServerProcess32);
    }

    hookData->ServerProcess32 = NULL;

    // Stop 64bit hooks.
    if (!UnhookWindowsHookEx(hookData->CbtHook))
        return perror("UnhookWindowsHookEx(CBTHooh)");

    hookData->CbtHook = NULL;

    if (!UnhookWindowsHookEx(hookData->CallWndHook))
        return perror("UnhookWindowsHookEx(CallWndHook)");

    hookData->CallWndHook = NULL;

    if (!UnhookWindowsHookEx(hookData->CallWndRetHook))
        return perror("UnhookWindowsHookEx(CallWndRetHook)");

    hookData->CallWndRetHook = NULL;

    if (!UnhookWindowsHookEx(hookData->GetMsgHook))
        return perror("UnhookWindowsHookEx(GetMsgHook)");

    hookData->GetMsgHook = NULL;

    hookData->HooksActive = FALSE;

    return ERROR_SUCCESS;
}

static BOOL CALLBACK AddWindowsProc(IN HWND window, IN LPARAM lParam)
{
    WINDOWINFO wi = { 0 };
    BANNED_WINDOWS *bannedWindows = (BANNED_WINDOWS *) lParam;
    ULONG status;

    LogVerbose("window %x", window);

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(window, &wi))
    {
        perror("GetWindowInfo");
        LogWarning("Skipping window %x", window);
        return TRUE;
    }

    if (!ShouldAcceptWindow(window, &wi))
        return TRUE; // skip

    if (bannedWindows)
    {
        if (bannedWindows->Explorer == window ||
            bannedWindows->Desktop == window ||
            bannedWindows->Start == window ||
            bannedWindows->Taskbar == window
            )
            return TRUE; // skip
    }

    status = AddWindowWithInfo(window, &wi, NULL);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "AddWindowWithInfo");
        return FALSE; // stop enumeration, fatal error occurred (should probably exit process at this point)
    }

    return TRUE;
}

// Adds all top-level windows to the watched list.
// watched windows critical section must be entered
static ULONG AddAllWindows(void)
{
    static BANNED_WINDOWS bannedWindows = { 0 };

    LogVerbose("start");

    // First, check for special windows that should be ignored.
    if (!bannedWindows.Explorer || !IsWindow(bannedWindows.Explorer))
        bannedWindows.Explorer = FindWindow(L"Progman", L"Program Manager");

    if (!bannedWindows.Taskbar || !IsWindow(bannedWindows.Taskbar))
    {
        bannedWindows.Taskbar = FindWindow(L"Shell_TrayWnd", NULL);

        if (bannedWindows.Taskbar)
        {
            if (g_SeamlessMode)
                ShowWindow(bannedWindows.Taskbar, SW_HIDE);
            else
                ShowWindow(bannedWindows.Taskbar, SW_SHOW);
        }
    }

    if (!bannedWindows.Start || !IsWindow(bannedWindows.Start))
    {
        bannedWindows.Start = FindWindowEx(g_DesktopWindow, NULL, L"Button", NULL);

        if (bannedWindows.Start)
        {
            if (g_SeamlessMode)
                ShowWindow(bannedWindows.Start, SW_HIDE);
            else
                ShowWindow(bannedWindows.Start, SW_SHOW);
        }
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopWindow, bannedWindows.Explorer, bannedWindows.Taskbar, bannedWindows.Start);

    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, (LPARAM) &bannedWindows))
        return perror("EnumWindows");

    return ERROR_SUCCESS;
}

// Reinitialize hooks/watched windows, called after a seamless/fullscreen switch or resolution change.
// NOTE: this function doesn't close/reopen qvideo's screen section
static ULONG ResetWatch(BOOL seamlessMode)
{
    WATCHED_DC *watchedDC;
    WATCHED_DC *nextWatchedDC;
    ULONG status;

    LogVerbose("start");

    status = StopHooks(&g_HookData);
    if (ERROR_SUCCESS != status)
        return perror2(status, "StopHooks");

    LogDebug("removing all windows");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    watchedDC = (WATCHED_DC *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WATCHED_DC *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WATCHED_DC, ListEntry);
        nextWatchedDC = (WATCHED_DC *) watchedDC->ListEntry.Flink;

        RemoveEntryList(&watchedDC->ListEntry);
        status = RemoveWindow(watchedDC);
        if (ERROR_SUCCESS != status)
        {
            LeaveCriticalSection(&g_csWatchedWindows);
            return perror2(status, "RemoveWindow");
        }

        watchedDC = nextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopWindow = NULL; // this causes reinitialization of "banned windows"

    status = ERROR_SUCCESS;

    // Only start hooks if we're in seamless mode.
    // FIXME: this means we rely on qvideo's damage notifications in fullscreen
    // WatchForEvents will map the whole screen as one window.
    if (seamlessMode)
    {
        status = StartHooks(&g_HookData);
        if (ERROR_SUCCESS != status)
            return perror2(status, "StartHooks");

        // Add all eligible windows to watch list.
        // Since this is a switch from fullscreen, no windows were watched.
        EnterCriticalSection(&g_csWatchedWindows);
        status = AddAllWindows();
        LeaveCriticalSection(&g_csWatchedWindows);
    }

    LogVerbose("success");
    return status;
}

// set fullscreen/seamless mode
ULONG SetSeamlessMode(IN BOOL seamlessMode, IN BOOL forceUpdate)
{
    ULONG status;

    LogVerbose("Seamless mode changing to %d", seamlessMode);

    if (g_SeamlessMode == seamlessMode && !forceUpdate)
        return ERROR_SUCCESS; // nothing to do

    CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, seamlessMode, NULL);

    if (!seamlessMode)
    {
        // show the screen window
        status = SendWindowMap(NULL);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowMap(NULL)");
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            LogDebug("Changing resolution to match host's");
            RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, 32, 0, 0);
            // FIXME: wait until the resolution actually changes?
        }
        // hide the screen window
        status = SendWindowUnmap(NULL);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowUnmap(NULL)");
    }

    // ResetWatch kills hooks if active and removes all watched windows.
    // If seamless mode is on, hooks are restarted and top-level windows are added to watch list.
    status = ResetWatch(seamlessMode);
    if (ERROR_SUCCESS != status)
        return perror2(status, "ResetWatch");

    g_SeamlessMode = seamlessMode;

    LogInfo("Seamless mode changed to %d", seamlessMode);

    return ERROR_SUCCESS;
}

WATCHED_DC *FindWindowByHandle(IN HWND window)
{
    WATCHED_DC *watchedDC;

    LogVerbose("%x", window);
    watchedDC = (WATCHED_DC *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WATCHED_DC *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WATCHED_DC, ListEntry);

        if (window == watchedDC->WindowHandle)
            return watchedDC;

        watchedDC = (WATCHED_DC *) watchedDC->ListEntry.Flink;
    }

    return NULL;
}

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
    LogVerbose("0x%x: seems OK", hwnd);
    return FALSE; // stop enumeration
}

// TODO: remove all this polling, handle changes in hook events
ULONG CheckWatchedWindowUpdates(
    IN OUT WATCHED_DC *watchedDC,
    IN const WINDOWINFO *windowInfo,
    IN BOOL damageDetected,
    IN const RECT *damageArea
    )
{
    WINDOWINFO wi;
    BOOL resizeDetected;
    BOOL moveDetected;
    BOOL currentlyVisible;
    BOOL updateStyle;
    MODAL_SEARCH_PARAMS modalParams;
    ULONG status;

    if (!watchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("window 0x%x", watchedDC->WindowHandle);

    if (!windowInfo)
    {
        wi.cbSize = sizeof(wi);
        if (!GetWindowInfo(watchedDC->WindowHandle, &wi))
            return perror("GetWindowInfo");
    }
    else
        memcpy(&wi, windowInfo, sizeof(wi));

    currentlyVisible = IsWindowVisible(watchedDC->WindowHandle);
    if (g_VchanClientConnected)
    {
        // visibility change
        if (currentlyVisible && !watchedDC->IsVisible)
        {
            status = SendWindowMap(watchedDC);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowMap");
        }

        if (!currentlyVisible && watchedDC->IsVisible)
        {
            status = SendWindowUnmap(watchedDC->WindowHandle);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowUnmap");
        }
    }

    if (!watchedDC->IsStyleChecked && (GetTickCount() >= watchedDC->TimeAdded + 500))
    {
        watchedDC->IsStyleChecked = TRUE;

        updateStyle = FALSE;
        if (wi.dwStyle & WS_MINIMIZEBOX)
        {
            wi.dwStyle &= ~WS_MINIMIZEBOX;
            updateStyle = TRUE;
            DeleteMenu(GetSystemMenu(watchedDC->WindowHandle, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
        }

        if (wi.dwStyle & WS_SIZEBOX)
        {
            wi.dwStyle &= ~WS_SIZEBOX;
            updateStyle = TRUE;
        }

        if (updateStyle)
        {
            SetWindowLong(watchedDC->WindowHandle, GWL_STYLE, wi.dwStyle);
            DrawMenuBar(watchedDC->WindowHandle);
        }
    }

    if ((wi.dwStyle & WS_DISABLED) && watchedDC->IsVisible && (GetTickCount() > watchedDC->TimeModalChecked + 500))
    {
        // possibly showing a modal window
        watchedDC->TimeModalChecked = GetTickCount();
        LogDebug("0x%x is WS_DISABLED, searching for modal window", watchedDC->WindowHandle);
        modalParams.ParentWindow = watchedDC->WindowHandle;
        modalParams.ModalWindow = NULL;

        if (!EnumWindows(FindModalChildProc, (LPARAM) &modalParams))
            return perror("EnumWindows");

        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            WATCHED_DC *modalDc = FindWindowByHandle(modalParams.ModalWindow);
            if (modalDc && !modalDc->ModalParent)
            {
                modalDc->ModalParent = watchedDC->WindowHandle;
                status = SendWindowUnmap(modalDc->WindowHandle);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowUnmap");

                status = SendWindowMap(modalDc);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowMap");
            }
        }
    }

    watchedDC->IsVisible = currentlyVisible;

    if (IsIconic(watchedDC->WindowHandle))
    {
        if (!watchedDC->IsIconic)
        {
            LogDebug("0x%x IsIconic: minimizing", watchedDC->WindowHandle);
            status = SendWindowFlags(watchedDC->WindowHandle, WINDOW_FLAG_MINIMIZE, 0);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowFlags");
            watchedDC->IsIconic = TRUE;
        }
        return ERROR_SUCCESS; // window is minimized, ignore everything else
    }
    else
    {
        LogVerbose("0x%x not iconic", watchedDC->WindowHandle);
        watchedDC->IsIconic = FALSE;
    }

    moveDetected = wi.rcWindow.left != watchedDC->WindowRect.left ||
        wi.rcWindow.top != watchedDC->WindowRect.top ||
        wi.rcWindow.right != watchedDC->WindowRect.right ||
        wi.rcWindow.bottom != watchedDC->WindowRect.bottom;

    damageDetected |= moveDetected;

    resizeDetected = (wi.rcWindow.right - wi.rcWindow.left != watchedDC->WindowRect.right - watchedDC->WindowRect.left) ||
        (wi.rcWindow.bottom - wi.rcWindow.top != watchedDC->WindowRect.bottom - watchedDC->WindowRect.top);

    if (damageDetected || resizeDetected)
    {
        watchedDC->WindowRect = wi.rcWindow;

        if (g_VchanClientConnected)
        {
            RECT intersection;

            if (moveDetected || resizeDetected)
            {
                status = SendWindowConfigure(watchedDC);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowConfigure");
            }

            if (damageArea == NULL)
            { // assume the whole area changed
                status = SendWindowDamageEvent(watchedDC->WindowHandle,
                    0,
                    0,
                    watchedDC->WindowRect.right - watchedDC->WindowRect.left,
                    watchedDC->WindowRect.bottom - watchedDC->WindowRect.top);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowDamageEvent");
            }
            else
            {
                // send only intersection of damage area and window area
                IntersectRect(&intersection, damageArea, &watchedDC->WindowRect);
                status = SendWindowDamageEvent(watchedDC->WindowHandle,
                    intersection.left - watchedDC->WindowRect.left,
                    intersection.top - watchedDC->WindowRect.top,
                    intersection.right - watchedDC->WindowRect.left,
                    intersection.bottom - watchedDC->WindowRect.top);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowDamageEvent");
            }
        }
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

BOOL ShouldAcceptWindow(IN HWND window, IN const WINDOWINFO *windowInfo OPTIONAL)
{
    WINDOWINFO wi;

    if (!windowInfo)
    {
        if (!GetWindowInfo(window, &wi))
        {
            perror("GetWindowInfo");
            return FALSE;
        }
        windowInfo = &wi;
    }

    // Don't skip invisible windows. We keep all windows in the list and map them when/if they become visible.
    //if (!IsWindowVisible(window))
    //    return FALSE;

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (windowInfo->dwStyle & WS_CHILD)
        return FALSE;

    // Office 2013 uses this style for some helper windows that are drawn on/near its border.
    // 0x800 exstyle is undocumented...
    // FIXME: ignoring these border "windows" causes weird window looks.
    // Investigate why moving main Office window doesn't move these windows.
    if (windowInfo->dwExStyle == (WS_EX_LAYERED | WS_EX_TOOLWINDOW | 0x800))
        return FALSE;

    return TRUE;
}

// Main function that scans for window updates.
// Called after receiving damage event from qvideo.
// TODO: remove this, handle changes in hook events (what about fullscreen?)
static ULONG ProcessUpdatedWindows(IN BOOL updateEverything, IN HDC screenDC)
{
    WATCHED_DC *watchedDC;
    WATCHED_DC *nextWatchedDC;
    static BANNED_WINDOWS bannedWindows = { 0 };
    BOOL recheckWindows = FALSE;
    HWND oldDesktopWindow = g_DesktopWindow;
    ULONG totalPages, page, dirtyPages = 0;
    RECT dirtyArea, currentArea;
    BOOL first = TRUE;

    if (g_UseDirtyBits)
    {
        totalPages = g_ScreenHeight * g_ScreenWidth * 4 / PAGE_SIZE;
        //debugf("update all? %d", bUpdateEverything);
        // create a damage rectangle from changed pages
        for (page = 0; page < totalPages; page++)
        {
            if (BIT_GET(g_DirtyPages->DirtyBits, page))
            {
                dirtyPages++;
                PageToRect(page, &currentArea);
                if (first)
                {
                    dirtyArea = currentArea;
                    first = FALSE;
                }
                else
                    UnionRect(&dirtyArea, &dirtyArea, &currentArea);
            }
        }

        // tell qvideo that we're done reading dirty bits
        SynchronizeDirtyBits(screenDC);

        LogDebug("DIRTY %d/%d (%d,%d)-(%d,%d)", dirtyPages, totalPages,
            dirtyArea.left, dirtyArea.top, dirtyArea.right, dirtyArea.bottom);

        if (dirtyPages == 0) // nothing changed according to qvideo
            return ERROR_SUCCESS;
    }

    AttachToInputDesktop();
    if (oldDesktopWindow != g_DesktopWindow)
    {
        recheckWindows = TRUE;
        LogDebug("desktop changed (old 0x%x), refreshing all windows", oldDesktopWindow);
        HideCursors();
        DisableEffects();
    }

    if (!bannedWindows.Explorer || recheckWindows || !IsWindow(bannedWindows.Explorer))
        bannedWindows.Explorer = FindWindow(NULL, L"Program Manager");

    if (!bannedWindows.Taskbar || recheckWindows || !IsWindow(bannedWindows.Taskbar))
    {
        bannedWindows.Taskbar = FindWindow(L"Shell_TrayWnd", NULL);

        if (bannedWindows.Taskbar)
        {
            if (g_SeamlessMode)
                ShowWindow(bannedWindows.Taskbar, SW_HIDE);
            else
                ShowWindow(bannedWindows.Taskbar, SW_SHOW);
        }
    }

    if (!bannedWindows.Start || recheckWindows || !IsWindow(bannedWindows.Start))
    {
        bannedWindows.Start = FindWindowEx(g_DesktopWindow, NULL, L"Button", NULL);

        if (bannedWindows.Start)
        {
            if (g_SeamlessMode)
                ShowWindow(bannedWindows.Start, SW_HIDE);
            else
                ShowWindow(bannedWindows.Start, SW_SHOW);
        }
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopWindow, bannedWindows.Explorer, bannedWindows.Taskbar, bannedWindows.Start);

    if (!g_SeamlessMode)
    {
        // just send damage event with the dirty area
        if (g_UseDirtyBits)
            SendWindowDamageEvent(0, dirtyArea.left, dirtyArea.top,
            dirtyArea.right - dirtyArea.left,
            dirtyArea.bottom - dirtyArea.top);
        else
            SendWindowDamageEvent(0, 0, 0, g_ScreenWidth, g_ScreenHeight);
        // TODO? if we're not using dirty bits we could narrow the damage area
        // by checking all windows... but it's probably not worth it.

        return ERROR_SUCCESS;
    }

    EnterCriticalSection(&g_csWatchedWindows);

    watchedDC = (WATCHED_DC *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WATCHED_DC *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WATCHED_DC, ListEntry);
        nextWatchedDC = (WATCHED_DC *) watchedDC->ListEntry.Flink;

        if (!IsWindow(watchedDC->WindowHandle) || !ShouldAcceptWindow(watchedDC->WindowHandle, NULL))
        {
            RemoveEntryList(&watchedDC->ListEntry);
            RemoveWindow(watchedDC);
            watchedDC = NULL;
        }
        else
        {
            if (g_UseDirtyBits)
            {
                if (IntersectRect(&currentArea, &dirtyArea, &watchedDC->WindowRect))
                    // skip windows that aren't in the changed area
                    CheckWatchedWindowUpdates(watchedDC, NULL, updateEverything, &dirtyArea);
            }
            else
                CheckWatchedWindowUpdates(watchedDC, NULL, updateEverything, NULL);
        }

        watchedDC = nextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    return ERROR_SUCCESS;
}

// Hook event: window created. Add it to the window list.
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

    status = AddWindowWithInfo((HWND) qhm->WindowHandle, &wi, NULL);
    if (ERROR_SUCCESS != status)
        perror2(status, "AddWindowWithInfo");

    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

static ULONG HookDestroyWindow(IN const QH_MESSAGE *qhm)
{
    WATCHED_DC *windowEntry;
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
    LogVerbose("%x '%s'", qhm->WindowHandle, qhm->Caption);

    return SendWindowName((HWND) qhm->WindowHandle, qhm->Caption);
}

// Process events from hooks.
// Updates watched windows state.
static ULONG HandleHookEvent(IN HANDLE hookIpc, IN OUT OVERLAPPED *hookAsyncState, IN QH_MESSAGE *qhm)
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

    default:
        break;
    }

    // TODO

    return status;
}

// main event loop
// TODO: refactor into smaller parts
static ULONG WINAPI WatchForEvents(void)
{
    HANDLE vchan, hookIpc;
    OVERLAPPED vchanAsyncState = { 0 }, hookIpcAsyncState = { 0 };
    unsigned int firedPort;
    ULONG eventCount;
    DWORD i, signaledEvent;
    BOOL vchanIoInProgress;
    ULONG status;
    BOOL exitLoop;
    HANDLE watchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE windowDamageEvent, fullScreenOnEvent, fullScreenOffEvent;
    HDC screenDC;
    ULONG damageNumber = 0;
    struct shm_cmd *shmCmd = NULL;
    QH_MESSAGE qhm;

    LogDebug("start");
    windowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    if (!VchanInitServer(6000))
    {
        LogError("VchanInitServer() failed");
        return GetLastError();
    }

    vchan = VchanGetHandle();

    vchanAsyncState.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    hookIpcAsyncState.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    fullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!fullScreenOnEvent)
        return GetLastError();
    fullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!fullScreenOffEvent)
        return GetLastError();
    g_ResolutionChangeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_ResolutionChangeEvent)
        return GetLastError();

    // Create IPC object for hook DLLs.
    hookIpc = CreateNamedMailslot(HOOK_IPC_NAME);
    if (!hookIpc)
        return perror("CreateNamedMailslot");

    g_VchanClientConnected = FALSE;
    vchanIoInProgress = FALSE;
    exitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize());

    while (TRUE)
    {
        watchedEvents[0] = g_ShutdownEvent;
        watchedEvents[1] = windowDamageEvent;
        watchedEvents[2] = fullScreenOnEvent;
        watchedEvents[3] = fullScreenOffEvent;
        watchedEvents[4] = g_ResolutionChangeEvent;
        watchedEvents[5] = hookIpcAsyncState.hEvent;

        status = ERROR_SUCCESS;

        VchanPrepareToSelect();
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!vchanIoInProgress && !ReadFile(vchan, &firedPort, 1, NULL, &vchanAsyncState))
        {
            status = GetLastError();
            if (ERROR_IO_PENDING != status)
            {
                perror("ReadFile");
                exitLoop = TRUE;
                break;
            }
        }

        vchanIoInProgress = TRUE;

        watchedEvents[6] = vchanAsyncState.hEvent;
        eventCount = 7;

        // Start hook mailslot async read.
        // Even if there is data available right away, processing is done in the event handler.
        status = ReadFile(hookIpc, &qhm, sizeof(qhm), NULL, &hookIpcAsyncState);

        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            status = perror("WaitForMultipleObjects");
            break;
        }

        if (0 == signaledEvent)
        {
            // shutdown event
            LogDebug("Shutdown event signaled");
            exitLoop = TRUE;
            break;
        }

        //debugf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
        switch (signaledEvent)
        {
        case 1: // damage event

            LogVerbose("Damage %d\n", damageNumber++);

            if (g_VchanClientConnected)
            {
                ProcessUpdatedWindows(TRUE, screenDC);
            }
            break;

        case 2: // seamless off event
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "SetSeamlessMode(FALSE)");
                exitLoop = TRUE;
            }
            break;

        case 3: // seamless on event
            status = SetSeamlessMode(TRUE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "SetSeamlessMode(TRUE)");
                exitLoop = TRUE;
            }
            break;

        case 4: // resolution change event, signaled by ResolutionChangeThread
            // Params are in g_ResolutionChangeParams
            status = ChangeResolution(&screenDC, windowDamageEvent);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "ChangeResolution");
                exitLoop = TRUE;
            }
            break;

        case 5: // mailslot read: message from our gui hook
            status = HandleHookEvent(hookIpc, &hookIpcAsyncState, &qhm);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "HandleHookEvent");
                exitLoop = TRUE;
            }
            break;

        case 6: // vchan receive
            // the following will never block; we need to do this to
            // clear libvchan_fd pending state
            //
            // using libvchan_wait here instead of reading fired
            // port at the beginning of the loop (ReadFile call) to be
            // sure that we clear pending state _only_
            // when handling vchan data in this loop iteration (not any
            // other process)
            if (!g_VchanClientConnected)
            {
                VchanWait();

                vchanIoInProgress = FALSE;

                LogInfo("A vchan client has connected\n");

                // Remove the xenstore device/vchan/N entry.
                if (!VchanIsServerConnected())
                {
                    LogError("VchanIsServerConnected() failed");
                    exitLoop = TRUE;
                    break;
                }

                if (ERROR_SUCCESS != SendProtocolVersion())
                {
                    LogError("SendProtocolVersion failed");
                    exitLoop = TRUE;
                    break;
                }

                // This will probably change the current video mode.
                if (ERROR_SUCCESS != HandleXconf())
                {
                    exitLoop = TRUE;
                    break;
                }

                // The screen DC should be opened only after the resolution changes.
                screenDC = GetDC(NULL);
                status = RegisterWatchedDC(screenDC, windowDamageEvent);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "RegisterWatchedDC");
                    exitLoop = TRUE;
                    break;
                }

                // send the whole screen framebuffer map
                status = SendWindowCreate(NULL);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendWindowCreate(NULL)");
                    exitLoop = TRUE;
                    break;
                }

                status = SendWindowMfns(NULL);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendWindowMfns(NULL)");
                    exitLoop = TRUE;
                    break;
                }

                g_VchanClientConnected = TRUE; // needs to be set before enumerating windows so maps get sent

                // This initializes watched windows, hooks etc.
                status = SetSeamlessMode(g_SeamlessMode, TRUE);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SetSeamlessMode");
                    exitLoop = TRUE;
                    break;
                }

                break;
            }

            if (!GetOverlappedResult(vchan, &vchanAsyncState, &i, FALSE))
            {
                if (GetLastError() == ERROR_IO_DEVICE)
                {
                    // in case of ring overflow, libvchan_wait
                    // will reset the evtchn ring, so ignore this
                    // error as already handled
                    //
                    // Overflow can happen when below loop ("while
                    // (read_ready_vchan_ext())") handle a lot of data
                    // in the same time as qrexec-daemon writes it -
                    // there where be no libvchan_wait call (which
                    // receive the events from the ring), but one will
                    // be signaled after each libvchan_write in
                    // qrexec-daemon. I don't know how to fix it
                    // properly (without introducing any race
                    // condition), so reset the evtchn ring (do not
                    // confuse with vchan ring, which stays untouched)
                    // in case of overflow.
                }
                else
                {
                    if (GetLastError() != ERROR_OPERATION_ABORTED)
                    {
                        perror("GetOverlappedResult(evtchn)");
                        exitLoop = TRUE;
                        break;
                    }
                }
            }

            EnterCriticalSection(&g_VchanCriticalSection);
            VchanWait();

            vchanIoInProgress = FALSE;

            if (VchanIsEof())
            {
                exitLoop = TRUE;
                break;
            }

            while (VchanGetReadBufferSize() > 0)
            {
                status = HandleServerData();
                if (ERROR_SUCCESS != status)
                {
                    exitLoop = TRUE;
                    LogError("handle_server_data() failed: 0x%x", status);
                    break;
                }
            }
            LeaveCriticalSection(&g_VchanCriticalSection);

            break;
        }

        if (exitLoop)
            break;
    }

    LogDebug("main loop finished");

    if (vchanIoInProgress)
    {
        if (CancelIo(vchan))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(vchanAsyncState.hEvent, INFINITE);
        }
    }

    if (!g_VchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        VchanIsServerConnected();
    }

    if (g_VchanClientConnected)
        VchanClose();

    StopHooks(&g_HookData); // don't care if it fails at this point

    CloseHandle(vchanAsyncState.hEvent);
    CloseHandle(windowDamageEvent);

    UnregisterWatchedDC(screenDC);
    CloseScreenSection();
    ReleaseDC(NULL, screenDC);
    LogInfo("exiting");

    return exitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static DWORD GetDomainName(OUT char *nameBuffer, IN DWORD nameLength)
{
    DWORD status = ERROR_SUCCESS;
    struct xs_handle *xs;
    char *domainName = NULL;

    xs = xs_domain_open();
    if (!xs)
    {
        LogError("Failed to open xenstore connection");
        status = ERROR_DEVICE_NOT_CONNECTED;
        goto cleanup;
    }

    domainName = xs_read(xs, XBT_NULL, "name", NULL);
    if (!domainName)
    {
        LogError("Failed to read domain name");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("%S", domainName);
    status = StringCchCopyA(nameBuffer, nameLength, domainName);
    if (FAILED(status))
    {
        perror2(status, "StringCchCopyA");
    }

cleanup:
    free(domainName);
    if (xs)
        xs_daemon_close(xs);

    return status;
}

static ULONG Init(void)
{
    ULONG status;
    WSADATA wsaData;
    WCHAR moduleName[CFG_MODULE_MAX];

    LogDebug("start");

    // This needs to be done first as a safeguard to not start multiple instances of this process.
    g_ShutdownEvent = CreateNamedEvent(WGA_SHUTDOWN_EVENT_NAME);
    if (!g_ShutdownEvent)
    {
        return GetLastError();
    }

    status = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));

    status = CfgReadDword(moduleName, REG_CONFIG_DIRTY_VALUE, &g_UseDirtyBits, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, disabling that feature", REG_CONFIG_DIRTY_VALUE);
        g_UseDirtyBits = FALSE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_CURSOR_VALUE, &g_DisableCursor, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_CURSOR_VALUE);
        g_DisableCursor = TRUE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_VALUE, &g_SeamlessMode, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_SEAMLESS_VALUE);
        g_SeamlessMode = TRUE;
    }

    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    status = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != status)
    {
        perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    SetLastError(status = CheckForXenInterface());
    if (ERROR_SUCCESS != status)
    {
        return perror("CheckForXenInterface");
    }

    // Read domain name from xenstore.
    status = GetDomainName(g_DomainName, RTL_NUMBER_OF(g_DomainName));
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read domain name from xenstore, using host name");

        status = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (status == 0)
        {
            if (0 != gethostname(g_DomainName, sizeof(g_DomainName)))
            {
                LogWarning("gethostname failed: 0x%x", status);
            }
            WSACleanup();
        }
        else
        {
            LogWarning("WSAStartup failed: 0x%x", status);
            // this is not fatal, only used to get host name for full desktop window title
        }
    }

    LogInfo("Fullscreen desktop name: %S", g_DomainName);

    InitializeListHead(&g_WatchedWindowsList);
    InitializeCriticalSection(&g_csWatchedWindows);
    return ERROR_SUCCESS;
}

int wmain(int argc, WCHAR *argv[])
{
    if (ERROR_SUCCESS != Init())
        return perror("Init");

    InitializeCriticalSection(&g_VchanCriticalSection);

    // Call the thread proc directly.
    if (ERROR_SUCCESS != WatchForEvents())
        return perror("WatchForEvents");

    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("exiting");
    return ERROR_SUCCESS;
}
