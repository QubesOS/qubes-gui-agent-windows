#include <windows.h>
#include <winsock2.h>
#include <stdlib.h>
#include <strsafe.h>

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

#define FULLSCREEN_ON_EVENT_NAME L"WGA_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"WGA_FULLSCREEN_OFF"

// If set, only invalidate parts of the screen that changed according to
// qvideo's dirty page scan of surface memory buffer.
BOOL g_bUseDirtyBits;

LONG g_ScreenHeight;
LONG g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;

BOOL g_SeamlessMode = TRUE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

char g_DomainName[256] = "<unknown>";

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

HANDLE g_hShellEventsThread = NULL;

HWND g_DesktopHwnd = NULL;
HWND g_ExplorerHwnd = NULL;
HWND g_TaskbarHwnd = NULL;
HWND g_StartButtonHwnd = NULL;

HANDLE g_ShutdownEvent = NULL;

ULONG ProcessUpdatedWindows(BOOL bUpdateEverything, HDC screenDC);

// can be called from main thread, shell hook thread or ResetWatch thread
ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC)
{
    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);
    free(pWatchedDC->pPfnArray);

    if (g_VchanClientConnected)
    {
        send_window_unmap(pWatchedDC->hWnd);
        if (pWatchedDC->hWnd) // never destroy screen "window"
            send_window_destroy(pWatchedDC->hWnd);
    }

    free(pWatchedDC);

    return ERROR_SUCCESS;
}


ULONG StartShellEventsThread(void)
{
    DWORD threadId;

    LogVerbose("start");

    if (g_hShellEventsThread)
    {
        LogError("shell events thread already running: handle 0x%x, window 0x%x", g_hShellEventsThread, g_ShellEventsWindow);
        // this is abnormal, returning error here will cause termination
        return ERROR_ALREADY_EXISTS;
    }

    g_hShellEventsThread = CreateThread(NULL, 0, ShellEventsThread, NULL, 0, &threadId);
    if (!g_hShellEventsThread)
        return perror("CreateThread(ShellEventsThread)");

    LogInfo("shell events thread ID: 0x%x (created)", threadId);
    return ERROR_SUCCESS;
}

ULONG StopShellEventsThread(void)
{
    ULONG retval;
    DWORD waitResult;

    LogVerbose("shell hook window: 0x%x", g_ShellEventsWindow);
    if (!g_hShellEventsThread)
        return ERROR_SUCCESS;

    // SendMessage waits until the message is processed
    if (!SendMessage(g_ShellEventsWindow, WM_CLOSE, 0, 0))
    {
        retval = perror("PostMessage(WM_CLOSE)");
        LogWarning("Terminating shell events thread forcibly");
        TerminateThread(g_hShellEventsThread, 0);
    }

    LogVerbose("waiting for thread to exit");
    waitResult = WaitForSingleObject(g_hShellEventsThread, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LogWarning("wait failed or timed out, killing thread forcibly");
        TerminateThread(g_hShellEventsThread, 0);
    }

    CloseHandle(g_hShellEventsThread);

    g_hShellEventsThread = NULL;
    g_ShellEventsWindow = NULL;

    LogDebug("shell events thread terminated");
    return ERROR_SUCCESS;
}

// Reinitialize everything, called after a session switch.
// This is executed as another thread to avoid shell events killing itself without finishing the job.
// TODO: use this with session change notification instead of AttachToInputDesktop every time?
// NOTE: this function doesn't close/reopen qvideo's screen section
DWORD WINAPI ResetWatch(PVOID param)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;

    LogVerbose("start");

    StopShellEventsThread();

    LogDebug("removing watches");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = (PWATCHED_DC)g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC)& g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC)pWatchedDC->le.Flink;

        RemoveEntryList(&pWatchedDC->le);
        RemoveWatchedDC(pWatchedDC);

        pWatchedDC = pNextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopHwnd = NULL;
    g_ExplorerHwnd = NULL;
    g_TaskbarHwnd = NULL;
    g_StartButtonHwnd = NULL;

    // todo: wait for desktop switch - it can take some time after the session event
    // (if using session switch event)

    // don't start shell events thread if we're in fullscreen mode
    // WatchForEvents will map the whole screen as one window
    if (g_SeamlessMode)
    {
        StartShellEventsThread();
        ProcessUpdatedWindows(TRUE, GetDC(NULL));
    }

    LogVerbose("success");
    return ERROR_SUCCESS;
}

// set fullscreen/seamless mode
static void SetFullscreenMode(void)
{
    LogInfo("Seamless mode changed to %d", g_SeamlessMode);

    // ResetWatch kills the shell event thread and removes all watched windows.
    // If fullscreen is off the shell event thread is also restarted.
    ResetWatch(NULL);

    if (!g_SeamlessMode)
    {
        // show the screen window
        send_window_map(NULL);
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            LogDebug("Changing resolution to match host's");
            RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, 32, 0, 0);
        }
        // hide the screen window
        send_window_unmap(NULL);
    }
}

PWATCHED_DC FindWindowByHwnd(HWND hWnd)
{
    PWATCHED_DC pWatchedDC;

    LogVerbose("%x", hWnd);
    pWatchedDC = (PWATCHED_DC)g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC)& g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);

        if (hWnd == pWatchedDC->hWnd)
            return pWatchedDC;

        pWatchedDC = (PWATCHED_DC)pWatchedDC->le.Flink;
    }

    return NULL;
}

// Enumerate top-level windows, searching for one that is modal
// in relation to a parent one (passed in lParam).
static BOOL WINAPI FindModalChildProc(
    IN HWND hwnd,
    IN LPARAM lParam
    )
{
    PMODAL_SEARCH_PARAMS msp = (PMODAL_SEARCH_PARAMS)lParam;
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

// can be called from main thread or shell hook thread
ULONG CheckWatchedWindowUpdates(
    PWATCHED_DC pWatchedDC,
    WINDOWINFO *pwi,
    BOOL bDamageDetected,
    PRECT prcDamageArea
    )
{
    WINDOWINFO wi;
    BOOL bResizingDetected;
    BOOL bMoveDetected;
    BOOL bCurrentlyVisible;
    BOOL bUpdateStyle;
    MODAL_SEARCH_PARAMS modalParams;

    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);

    if (!pwi)
    {
        wi.cbSize = sizeof(wi);
        if (!GetWindowInfo(pWatchedDC->hWnd, &wi))
            return perror("GetWindowInfo");
    }
    else
        memcpy(&wi, pwi, sizeof(wi));

    bCurrentlyVisible = IsWindowVisible(pWatchedDC->hWnd);
    if (g_VchanClientConnected)
    {
        // visibility change
        if (bCurrentlyVisible && !pWatchedDC->bVisible)
            send_window_map(pWatchedDC);

        if (!bCurrentlyVisible && pWatchedDC->bVisible)
            send_window_unmap(pWatchedDC->hWnd);
    }

    if (!pWatchedDC->bStyleChecked && (GetTickCount() >= pWatchedDC->uTimeAdded + 500))
    {
        pWatchedDC->bStyleChecked = TRUE;

        bUpdateStyle = FALSE;
        if (wi.dwStyle & WS_MINIMIZEBOX)
        {
            wi.dwStyle &= ~WS_MINIMIZEBOX;
            bUpdateStyle = TRUE;
            DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
        }

        if (wi.dwStyle & WS_SIZEBOX)
        {
            wi.dwStyle &= ~WS_SIZEBOX;
            bUpdateStyle = TRUE;
        }

        if (bUpdateStyle)
        {
            SetWindowLong(pWatchedDC->hWnd, GWL_STYLE, wi.dwStyle);
            DrawMenuBar(pWatchedDC->hWnd);
        }
    }

    if ((wi.dwStyle & WS_DISABLED) && pWatchedDC->bVisible && (GetTickCount() > pWatchedDC->uTimeModalChecked + 500))
    {
        // possibly showing a modal window
        pWatchedDC->uTimeModalChecked = GetTickCount();
        LogDebug("0x%x is WS_DISABLED, searching for modal window", pWatchedDC->hWnd);
        modalParams.ParentWindow = pWatchedDC->hWnd;
        modalParams.ModalWindow = NULL;
        EnumWindows(FindModalChildProc, (LPARAM)&modalParams);
        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            PWATCHED_DC modalDc = FindWindowByHwnd(modalParams.ModalWindow);
            if (modalDc && !modalDc->ModalParent)
            {
                modalDc->ModalParent = pWatchedDC->hWnd;
                send_window_unmap(modalDc->hWnd);
                send_window_map(modalDc);
            }
        }
    }

    pWatchedDC->bVisible = bCurrentlyVisible;

    if (IsIconic(pWatchedDC->hWnd))
    {
        if (!pWatchedDC->bIconic)
        {
            LogDebug("0x%x IsIconic: minimizing", pWatchedDC->hWnd);
            send_window_flags(pWatchedDC->hWnd, WINDOW_FLAG_MINIMIZE, 0);
            pWatchedDC->bIconic = TRUE;
        }
        return ERROR_SUCCESS; // window is minimized, ignore everything else
    }
    else
    {
        LogVerbose("0x%x not iconic", pWatchedDC->hWnd);
        pWatchedDC->bIconic = FALSE;
    }

    bMoveDetected = wi.rcWindow.left != pWatchedDC->rcWindow.left ||
        wi.rcWindow.top != pWatchedDC->rcWindow.top ||
        wi.rcWindow.right != pWatchedDC->rcWindow.right ||
        wi.rcWindow.bottom != pWatchedDC->rcWindow.bottom;

    bDamageDetected |= bMoveDetected;

    bResizingDetected = (wi.rcWindow.right - wi.rcWindow.left != pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left) ||
        (wi.rcWindow.bottom - wi.rcWindow.top != pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);

    if (bDamageDetected || bResizingDetected)
    {
        //		_tprintf(_T("hwnd: %x, left: %d top: %d right: %d bottom %d\n"), pWatchedDC->hWnd, wi.rcWindow.left, wi.rcWindow.top, wi.rcWindow.right,
        //			 wi.rcWindow.bottom);
        pWatchedDC->rcWindow = wi.rcWindow;

        if (g_VchanClientConnected)
        {
            RECT intersection;

            if (bMoveDetected || bResizingDetected)
                send_window_configure(pWatchedDC);

            if (prcDamageArea == NULL)
            { // assume the whole area changed
                send_window_damage_event(pWatchedDC->hWnd,
                    0,
                    0,
                    pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left,
                    pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);
            }
            else
            {
                // send only intersection of damage area and window area
                IntersectRect(&intersection, prcDamageArea, &pWatchedDC->rcWindow);
                send_window_damage_event(pWatchedDC->hWnd,
                    intersection.left - pWatchedDC->rcWindow.left,
                    intersection.top - pWatchedDC->rcWindow.top,
                    intersection.right - pWatchedDC->rcWindow.left,
                    intersection.bottom - pWatchedDC->rcWindow.top);
            }
        }
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

BOOL ShouldAcceptWindow(HWND hWnd, OPTIONAL WINDOWINFO *pwi)
{
    WINDOWINFO wi;

    if (!pwi)
    {
        if (!GetWindowInfo(hWnd, &wi))
            return FALSE;
        pwi = &wi;
    }

    //LogVerbose("0x%x: %x %x", hWnd, pwi->dwStyle, pwi->dwExStyle);
    if (!IsWindowVisible(hWnd))
        return FALSE;

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (pwi->dwStyle & WS_CHILD)
        return FALSE;

    // Office 2013 uses this style for some helper windows that are drawn on/near its border.
    // 0x800 exstyle is undocumented...
    if (pwi->dwExStyle == (WS_EX_LAYERED | WS_EX_TOOLWINDOW | 0x800))
        return FALSE;

    return TRUE;
}

// Enumerate top-level windows and add them to the watch list.
BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    WINDOWINFO wi;
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)lParam;
    ULONG i;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return TRUE;

    if (!ShouldAcceptWindow(hWnd, &wi))
        return TRUE;

    if (pBannedPopupsList)
    {
        for (i = 0; i < pBannedPopupsList->uNumberOfBannedPopups; i++)
        {
            if (pBannedPopupsList->hBannedPopupArray[i] == hWnd)
                return TRUE;
        }
    }

    AddWindowWithInfo(hWnd, &wi);

    return TRUE;
}

// Main function that scans for window updates.
// Called after receiving damage event from qvideo.
static ULONG ProcessUpdatedWindows(BOOL bUpdateEverything, HDC screenDC)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;
    BYTE BannedPopupsListBuffer[sizeof(BANNED_POPUP_WINDOWS)* 4];
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)&BannedPopupsListBuffer;
    BOOL bRecheckWindows = FALSE;
    HWND hwndOldDesktop = g_DesktopHwnd;
    ULONG uTotalPages, uPage, uDirtyPages = 0;
    RECT rcDirtyArea, rcCurrent;
    BOOL bFirst = TRUE;

    if (g_bUseDirtyBits)
    {
        uTotalPages = g_ScreenHeight * g_ScreenWidth * 4 / PAGE_SIZE;
        //debugf("update all? %d", bUpdateEverything);
        // create a damage rectangle from changed pages
        for (uPage = 0; uPage < uTotalPages; uPage++)
        {
            if (BIT_GET(g_pDirtyPages->DirtyBits, uPage))
            {
                uDirtyPages++;
                PageToRect(uPage, &rcCurrent);
                if (bFirst)
                {
                    rcDirtyArea = rcCurrent;
                    bFirst = FALSE;
                }
                else
                    UnionRect(&rcDirtyArea, &rcDirtyArea, &rcCurrent);
            }
        }

        // tell qvideo that we're done reading dirty bits
        SynchronizeDirtyBits(screenDC);

        LogDebug("DIRTY %d/%d (%d,%d)-(%d,%d)", uDirtyPages, uTotalPages,
            rcDirtyArea.left, rcDirtyArea.top, rcDirtyArea.right, rcDirtyArea.bottom);

        if (uDirtyPages == 0) // nothing changed according to qvideo
            return ERROR_SUCCESS;
    }

    AttachToInputDesktop();
    if (hwndOldDesktop != g_DesktopHwnd)
    {
        bRecheckWindows = TRUE;
        LogDebug("desktop changed (old 0x%x), refreshing all windows", hwndOldDesktop);
        HideCursors();
        DisableEffects();
    }

    if (!g_ExplorerHwnd || bRecheckWindows || !IsWindow(g_ExplorerHwnd))
        g_ExplorerHwnd = FindWindow(NULL, L"Program Manager");

    if (!g_TaskbarHwnd || bRecheckWindows || !IsWindow(g_TaskbarHwnd))
    {
        g_TaskbarHwnd = FindWindow(L"Shell_TrayWnd", NULL);

        if (g_TaskbarHwnd)
        {
            if (g_SeamlessMode)
                ShowWindow(g_TaskbarHwnd, SW_HIDE);
            else
                ShowWindow(g_TaskbarHwnd, SW_SHOW);
        }
    }

    if (!g_StartButtonHwnd || bRecheckWindows || !IsWindow(g_StartButtonHwnd))
    {
        g_StartButtonHwnd = FindWindowEx(g_DesktopHwnd, NULL, L"Button", NULL);

        if (g_StartButtonHwnd)
        {
            if (g_SeamlessMode)
                ShowWindow(g_StartButtonHwnd, SW_HIDE);
            else
                ShowWindow(g_StartButtonHwnd, SW_SHOW);
        }
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopHwnd, g_ExplorerHwnd, g_TaskbarHwnd, g_StartButtonHwnd);

    if (!g_SeamlessMode)
    {
        // just send damage event with the dirty area
        if (g_bUseDirtyBits)
            send_window_damage_event(0, rcDirtyArea.left, rcDirtyArea.top,
            rcDirtyArea.right - rcDirtyArea.left,
            rcDirtyArea.bottom - rcDirtyArea.top);
        else
            send_window_damage_event(0, 0, 0, g_ScreenWidth, g_ScreenHeight);
        // TODO? if we're not using dirty bits we could narrow the damage area
        // by checking all windows... but it's probably not worth it.

        return ERROR_SUCCESS;
    }

    pBannedPopupsList->uNumberOfBannedPopups = 4;
    pBannedPopupsList->hBannedPopupArray[0] = g_DesktopHwnd;
    pBannedPopupsList->hBannedPopupArray[1] = g_ExplorerHwnd;
    pBannedPopupsList->hBannedPopupArray[2] = g_TaskbarHwnd;
    pBannedPopupsList->hBannedPopupArray[3] = g_StartButtonHwnd;

    EnterCriticalSection(&g_csWatchedWindows);

    EnumWindows(EnumWindowsProc, (LPARAM)pBannedPopupsList);

    pWatchedDC = (PWATCHED_DC)g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC)&g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC)pWatchedDC->le.Flink;

        if (!IsWindow(pWatchedDC->hWnd) || !ShouldAcceptWindow(pWatchedDC->hWnd, NULL))
        {
            RemoveEntryList(&pWatchedDC->le);
            RemoveWatchedDC(pWatchedDC);
            pWatchedDC = NULL;
        }
        else
        {
            if (g_bUseDirtyBits)
            {
                if (IntersectRect(&rcCurrent, &rcDirtyArea, &pWatchedDC->rcWindow))
                    // skip windows that aren't in the changed area
                    CheckWatchedWindowUpdates(pWatchedDC, NULL, bUpdateEverything, &rcDirtyArea);
            }
            else
                CheckWatchedWindowUpdates(pWatchedDC, NULL, bUpdateEverything, NULL);
        }

        pWatchedDC = pNextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    return ERROR_SUCCESS;
}

// g_csWatchedWindows critical section must be entered
PWATCHED_DC AddWindowWithInfo(HWND hWnd, WINDOWINFO *pwi)
{
    PWATCHED_DC pWatchedDC = NULL;

    if (!pwi)
        return NULL;

    LogDebug("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x",
        hWnd, pwi->rcWindow.left, pwi->rcWindow.top, pwi->rcWindow.right, pwi->rcWindow.bottom, pwi->dwStyle, pwi->dwExStyle);

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (pWatchedDC)
        // already being watched
        return pWatchedDC;

    if ((pwi->rcWindow.top - pwi->rcWindow.bottom == 0) || (pwi->rcWindow.right - pwi->rcWindow.left == 0))
        return NULL;

    pWatchedDC = (PWATCHED_DC)malloc(sizeof(WATCHED_DC));
    if (!pWatchedDC)
        return NULL;

    ZeroMemory(pWatchedDC, sizeof(WATCHED_DC));

    pWatchedDC->bVisible = IsWindowVisible(hWnd);
    pWatchedDC->bIconic = IsIconic(hWnd);

    pWatchedDC->bStyleChecked = FALSE;
    pWatchedDC->uTimeAdded = pWatchedDC->uTimeModalChecked = GetTickCount();

    LogDebug("0x%x: visible=%d, iconic=%d", pWatchedDC->hWnd, pWatchedDC->bVisible, pWatchedDC->bIconic);

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (pwi->rcWindow.right - pwi->rcWindow.left == g_ScreenWidth
        && pwi->rcWindow.bottom - pwi->rcWindow.top == g_ScreenHeight)
    {
        LogDebug("popup too large: %dx%d, screen %dx%d",
            pwi->rcWindow.right - pwi->rcWindow.left,
            pwi->rcWindow.bottom - pwi->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
        pWatchedDC->bOverrideRedirect = FALSE;
    }
    else
    {
        // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
        if ((pwi->dwStyle & WS_CAPTION) == WS_CAPTION) // normal window
            pWatchedDC->bOverrideRedirect = FALSE;
        else if (((pwi->dwStyle & WS_SYSMENU) == WS_SYSMENU) && ((pwi->dwExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
            // Metro apps without WS_CAPTION.
            // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
            // but I guess MS doesn't adhere to its own standards...
            pWatchedDC->bOverrideRedirect = FALSE;
        else
            pWatchedDC->bOverrideRedirect = TRUE;
    }

    if (pWatchedDC->bOverrideRedirect)
        LogDebug("popup: %dx%d, screen %dx%d",
            pwi->rcWindow.right - pwi->rcWindow.left,
            pwi->rcWindow.bottom - pwi->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);


    pWatchedDC->hWnd = hWnd;
    pWatchedDC->rcWindow = pwi->rcWindow;

    pWatchedDC->pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));

    pWatchedDC->MaxWidth = g_ScreenWidth;
    pWatchedDC->MaxHeight = g_ScreenHeight;

    if (g_VchanClientConnected)
    {
        send_window_create(pWatchedDC);
        send_wmname(hWnd);
    }

    InsertTailList(&g_WatchedWindowsList, &pWatchedDC->le);

    return pWatchedDC;
}

// main event loop
static ULONG WINAPI WatchForEvents(void)
{
    HANDLE vchan, mailslot;
    OVERLAPPED olVchan, olMailslot;
    unsigned int fired_port;
    ULONG eventCount;
    DWORD i, dwSignaledEvent, size;
    BOOL bVchanIoInProgress;
    ULONG uResult;
    BOOL bExitLoop;
    HANDLE WatchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE hWindowDamageEvent;
    HANDLE hFullScreenOnEvent;
    HANDLE hFullScreenOffEvent;
    HDC screenDC;
    ULONG uDamage = 0;
    struct shm_cmd *pShmCmd = NULL;
    QH_MESSAGE qhm;
    HANDLE hookServerProcess;
    HANDLE hookShutdownEvent;

    LogDebug("start");
    hWindowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    if (!VchanInitServer(6000))
    {
        LogError("VchanInitServer() failed");
        return GetLastError();
    }

    vchan = VchanGetHandle();

    ZeroMemory(&olVchan, sizeof(olVchan));
    olVchan.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    ZeroMemory(&olMailslot, sizeof(olMailslot));
    olMailslot.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    hookShutdownEvent = CreateNamedEvent(WGA32_SHUTDOWN_EVENT_NAME);
    if (!hookShutdownEvent)
        return GetLastError();
    hFullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!hFullScreenOnEvent)
        return GetLastError();
    hFullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!hFullScreenOffEvent)
        return GetLastError();
    g_ResolutionChangeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_ResolutionChangeEvent)
        return GetLastError();

    // Create IPC object for hook DLLs.
    mailslot = CreateMailslot(HOOK_IPC_NAME, 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (!mailslot)
        return perror("CreateMailslot");

    // Start 64-bit hooks.
    if (ERROR_SUCCESS != SetHooks(HOOK_DLL_NAME_64))
        return GetLastError();

    // Start the 32-bit hook server. It exits when the wga shutdown event is signaled.
    if (ERROR_SUCCESS != StartProcess(HOOK_SERVER_NAME_32, &hookServerProcess))
        return GetLastError();

    g_VchanClientConnected = FALSE;
    bVchanIoInProgress = FALSE;
    bExitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize());

    while (TRUE)
    {
        eventCount = 0;

        // Order matters.
        WatchedEvents[eventCount++] = g_ShutdownEvent;
        WatchedEvents[eventCount++] = hWindowDamageEvent;
        WatchedEvents[eventCount++] = hFullScreenOnEvent;
        WatchedEvents[eventCount++] = hFullScreenOffEvent;
        WatchedEvents[eventCount++] = g_ResolutionChangeEvent;
        WatchedEvents[eventCount++] = olMailslot.hEvent;

        uResult = ERROR_SUCCESS;

        VchanPrepareToSelect();
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!bVchanIoInProgress && !ReadFile(vchan, &fired_port, 1, NULL, &olVchan))
        {
            uResult = GetLastError();
            if (ERROR_IO_PENDING != uResult)
            {
                perror("ReadFile");
                bExitLoop = TRUE;
                break;
            }
        }

        bVchanIoInProgress = TRUE;

        WatchedEvents[eventCount++] = olVchan.hEvent;

        // Start hook maislot async read.
        // Even if there is data available right away, processing is done in the event handler.
        uResult = ReadFile(mailslot, &qhm, sizeof(qhm), NULL, &olMailslot);

        dwSignaledEvent = WaitForMultipleObjects(eventCount, WatchedEvents, FALSE, INFINITE);
        if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            uResult = perror("WaitForMultipleObjects");
            break;
        }
        else
        {
            if (0 == dwSignaledEvent)
            {
                // shutdown event
                logf("Shutdown event signaled");
                bExitLoop = TRUE;
                break;
            }

            //debugf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
            switch (dwSignaledEvent)
            {
            case 1: // damage event

                LogVerbose("Damage %d\n", uDamage++);

                if (g_VchanClientConnected)
                {
                    ProcessUpdatedWindows(TRUE, screenDC);
                }
                break;

            case 2: // fullscreen on event
                if (!g_SeamlessMode)
                    break; // already in fullscreen
                g_SeamlessMode = FALSE;
                CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, g_SeamlessMode, NULL);
                SetFullscreenMode();
                break;

            case 3: // fullscreen off event
                if (g_SeamlessMode)
                    break; // already in seamless
                g_SeamlessMode = TRUE;
                CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, g_SeamlessMode, NULL);
                SetFullscreenMode();
                break;

            case 4: // resolution change event, signaled by ResolutionChangeThread
                // Params are in g_ResolutionChangeParams
                ChangeResolution(&screenDC, hWindowDamageEvent);
                break;

            case 5: // mailslot read: message from our gui hook
                if (!GetOverlappedResult(mailslot, &olMailslot, &size, FALSE))
                {
                    perror("GetOverlappedResult(mailslot)");
                    bExitLoop = TRUE;
                    break;
                }

                if (size != sizeof(qhm))
                {
                    LogWarning("Invalid hook message size: %d (expected %d)", size, sizeof(qhm));
                    // non-fatal although shouldn't happen
                    break;
                }
                LogDebug("%8x: %4x %8x %8x\n",
                    qhm.WindowHandle,
                    qhm.Message,
                    //qhm.HookId == WH_CBT ? CBTNameFromId(qhm.Message) : MsgNameFromId(qhm.Message),
                    qhm.wParam, qhm.lParam);
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

                    bVchanIoInProgress = FALSE;

                    LogInfo("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    if (!VchanIsServerConnected())
                    {
                        LogError("VchanIsServerConnected() failed");
                        bExitLoop = TRUE;
                        break;
                    }

                    send_protocol_version();

                    // This will probably change the current video mode.
                    if (ERROR_SUCCESS != handle_xconf())
                    {
                        bExitLoop = TRUE;
                        break;
                    }

                    // The screen DC should be opened only after the resolution changes.
                    screenDC = GetDC(NULL);
                    uResult = RegisterWatchedDC(screenDC, hWindowDamageEvent);
                    if (ERROR_SUCCESS != uResult)
                    {
                        perror("RegisterWatchedDC");
                        bExitLoop = TRUE;
                        break;
                    }

                    // send the whole screen framebuffer map
                    send_window_create(NULL);
                    send_pixmap_mfns(NULL);

                    if (!g_SeamlessMode)
                    {
                        LogInfo("init in fullscreen mode");
                        send_window_map(NULL);
                    }
                    else
                    {
                        if (ERROR_SUCCESS != StartShellEventsThread())
                        {
                            LogError("StartShellEventsThread failed, exiting");
                            bExitLoop = TRUE;
                            break;
                        }
                    }

                    g_VchanClientConnected = TRUE;
                    break;
                }

                if (!GetOverlappedResult(vchan, &olVchan, &i, FALSE))
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
                        if (GetLastError() != ERROR_OPERATION_ABORTED)
                        {
                            perror("GetOverlappedResult(evtchn)");
                            bExitLoop = TRUE;
                            break;
                        }
                }

                EnterCriticalSection(&g_VchanCriticalSection);
                VchanWait();

                bVchanIoInProgress = FALSE;

                if (VchanIsEof())
                {
                    bExitLoop = TRUE;
                    break;
                }

                while (VchanGetReadBufferSize())
                {
                    uResult = handle_server_data();
                    if (ERROR_SUCCESS != uResult)
                    {
                        bExitLoop = TRUE;
                        LogError("handle_server_data() failed: 0x%x", uResult);
                        break;
                    }
                }
                LeaveCriticalSection(&g_VchanCriticalSection);

                break;
            }
        }

        if (bExitLoop)
            break;
    }

    LogDebug("main loop finished");

    if (bVchanIoInProgress)
        if (CancelIo(vchan))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(olVchan.hEvent, INFINITE);
        }

    if (!g_VchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        VchanIsServerConnected();
    }

    if (g_VchanClientConnected)
        VchanClose();

    // Shutdown QGuiHookServer32.
    SetEvent(hookShutdownEvent);

    if (WAIT_OBJECT_0 != WaitForSingleObject(hookServerProcess, 1000))
    {
        LogWarning("QGuiHookServer32 didn't exit in time, killing it");
        TerminateProcess(hookServerProcess, 0);
    }

    CloseHandle(olVchan.hEvent);
    CloseHandle(hWindowDamageEvent);

    StopShellEventsThread();
    UnregisterWatchedDC(screenDC);
    CloseScreenSection();
    ReleaseDC(NULL, screenDC);
    LogInfo("exiting");

    return bExitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static DWORD GetDomainName(char *nameBuffer, DWORD nameLength)
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
    ULONG uResult;
    WSADATA wsaData;
    WCHAR moduleName[CFG_MODULE_MAX];

    LogDebug("start");

    // This needs to be done first as a safeguard to not start multiple instances of this process.
    g_ShutdownEvent = CreateNamedEvent(WGA_SHUTDOWN_EVENT_NAME);
    if (!g_ShutdownEvent)
    {
        return GetLastError();
    }

    uResult = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));

    uResult = CfgReadDword(moduleName, REG_CONFIG_DIRTY_VALUE, &g_bUseDirtyBits, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        LogWarning("Failed to read '%s' config value, disabling that feature", REG_CONFIG_DIRTY_VALUE);
        g_bUseDirtyBits = FALSE;
    }

    uResult = CfgReadDword(moduleName, REG_CONFIG_CURSOR_VALUE, &g_DisableCursor, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_CURSOR_VALUE);
        g_DisableCursor = TRUE;
    }

    uResult = CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_VALUE, &g_SeamlessMode, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_SEAMLESS_VALUE);
        g_SeamlessMode = TRUE;
    }

    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    uResult = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != uResult)
    {
        perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    SetLastError(uResult = CheckForXenInterface());
    if (ERROR_SUCCESS != uResult)
    {
        return perror("CheckForXenInterface");
    }

    // Read domain name from xenstore.
    uResult = GetDomainName(g_DomainName, RTL_NUMBER_OF(g_DomainName));
    if (ERROR_SUCCESS != uResult)
    {
        LogWarning("Failed to read domain name from xenstore, using host name");

        uResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (uResult == 0)
        {
            if (0 != gethostname(g_DomainName, sizeof(g_DomainName)))
            {
                LogWarning("gethostname failed: 0x%x", uResult);
            }
            WSACleanup();
        }
        else
        {
            LogWarning("WSAStartup failed: 0x%x", uResult);
            // this is not fatal, only used to get host name for full desktop window title
        }
    }

    LogInfo("Fullscreen desktop name: %S", g_DomainName);

    InitializeListHead(&g_WatchedWindowsList);
    InitializeCriticalSection(&g_csWatchedWindows);
    return ERROR_SUCCESS;
}

int wmain(ULONG argc, PWCHAR argv[])
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
