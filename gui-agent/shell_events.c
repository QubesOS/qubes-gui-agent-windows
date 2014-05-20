#include "shell_events.h"
#include "log.h"

// If set, only invalidate parts of the screen that changed according to
// qvideo's dirty page scan of surface memory buffer.
BOOL g_bUseDirtyBits = FALSE;

const WCHAR g_szClassName[] = L"QubesShellHookClass";
ULONG g_uShellHookMessage = 0;
HWND g_ShellEventsWnd = NULL;
HANDLE g_hShellEventsThread = NULL;

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;
BOOL g_Initialized = FALSE;

LONG g_ScreenHeight = 0;
LONG g_ScreenWidth = 0;

HWND g_DesktopHwnd = NULL;
HWND g_ExplorerHwnd = NULL;
HWND g_TaskbarHwnd = NULL;
HWND g_StartButtonHwnd = NULL;

PWATCHED_DC AddWindowWithInfo(
    HWND hWnd,
    WINDOWINFO *pwi
    );

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC);

PWATCHED_DC FindWindowByHwnd(HWND hWnd)
{
    PWATCHED_DC pWatchedDC;

    debugf("%x", hWnd);
    pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);

        if (hWnd == pWatchedDC->hWnd)
            return pWatchedDC;

        pWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;
    }

    return NULL;
}

// Enumerate top-level windows, searching for one that is modal
// in relation to a parent one (passed in lParam).
BOOL WINAPI FindModalChildProc(
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
    debugf("0x%x: seems OK", hwnd);
    return FALSE; // stop enumeration
}

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

    debugf("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);

    if (!pwi)
    {
        wi.cbSize = sizeof(wi);
        if (!GetWindowInfo(pWatchedDC->hWnd, &wi))
            return perror("GetWindowInfo");
    }
    else
        memcpy(&wi, pwi, sizeof(wi));

    bCurrentlyVisible = IsWindowVisible(pWatchedDC->hWnd);
    if (g_bVchanClientConnected)
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
        debugf("0x%x is WS_DISABLED, searching for modal window", pWatchedDC->hWnd);
        modalParams.ParentWindow = pWatchedDC->hWnd;
        modalParams.ModalWindow = NULL;
        EnumWindows(FindModalChildProc, (LPARAM)&modalParams);
        debugf("result: 0x%x", modalParams.ModalWindow);
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

        if (g_bVchanClientConnected)
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

    //debugf("0x%x: %x %x", hWnd, pwi->dwStyle, pwi->dwExStyle);
    if (!IsWindowVisible(hWnd))
        return FALSE;
    
    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (pwi->dwStyle & WS_CHILD)
        return FALSE;

    // Office 2013 uses this style for some helper windows that are drawn on/near its border.
    // 0x800 exstyle is undocumented...
    if (pwi->dwExStyle == (WS_EX_LAYERED|WS_EX_TOOLWINDOW|0x800))
        return FALSE;

    return TRUE;
}

// Enumerate top-level windows and add them to the watch list.
BOOL CALLBACK EnumWindowsProc(
    HWND hWnd,
    LPARAM lParam
    )
{
    WINDOWINFO wi;
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS) lParam;
    ULONG i;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return TRUE;

    if (!ShouldAcceptWindow(hWnd, &wi))
        return TRUE;

    if (pBannedPopupsList)
        for (i = 0; i < pBannedPopupsList->uNumberOfBannedPopups; i++)
            if (pBannedPopupsList->hBannedPopupArray[i] == hWnd)
                return TRUE;

    AddWindowWithInfo(hWnd, &wi);

    return TRUE;
}

// Set current thread's desktop to the current input desktop.
ULONG AttachToInputDesktop()
{
    ULONG uResult = ERROR_SUCCESS;
    HDESK desktop = 0, oldDesktop = 0;
    WCHAR name[256];
    DWORD needed;
    DWORD sessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    //debugf("start");
    desktop = OpenInputDesktop(0, FALSE,
        DESKTOP_CREATEMENU|DESKTOP_CREATEWINDOW|DESKTOP_ENUMERATE|DESKTOP_HOOKCONTROL
        |DESKTOP_JOURNALPLAYBACK|DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS);

    if (!desktop)
    {
        uResult = perror("OpenInputDesktop");
        goto cleanup;
    }

#ifdef DEBUG
    if (!GetUserObjectInformation(desktop, UOI_NAME, name, sizeof(name), &needed))
    {
        perror("GetUserObjectInformation");
    }
    else
    {
        // Get access token from ourselves.
        OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
        // Session ID is stored in the access token.
        GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size);
        CloseHandle(currentToken);
        debugf("current input desktop: %s, current session: %d, console session: %d",
            name, sessionId, WTSGetActiveConsoleSessionId());
    }
#endif

    // Close old handle to prevent object leaks.
    oldDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop(desktop))
    {
        uResult = perror("SetThreadDesktop");
        goto cleanup;
    }

    g_DesktopHwnd = GetDesktopWindow();

cleanup:
    if (oldDesktop)
        if (!CloseDesktop(oldDesktop))
            perror("CloseDesktop(previous)");
    //debugf("result: %d", uResult);
    return uResult;
}

// Convert memory page number in the screen buffer to a rectangle that covers it.
void PageToRect(ULONG uPageNumber, OUT PRECT pRect)
{
    ULONG uStride = g_ScreenWidth * 4;
    ULONG uPageStart = uPageNumber * PAGE_SIZE;

    pRect->left = (uPageStart % uStride) / 4;
    pRect->top = uPageStart / uStride;
    pRect->right = ((uPageStart+PAGE_SIZE-1) % uStride) / 4;
    pRect->bottom = (uPageStart+PAGE_SIZE-1) / uStride;

    if (pRect->left > pRect->right) // page crossed right border
    {
        pRect->left = 0;
        pRect->right = g_ScreenWidth-1;
    }
}

// Main function that scans for window updates.
// Runs in the main thread.
ULONG ProcessUpdatedWindows(BOOL bUpdateEverything)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;
    BYTE BannedPopupsListBuffer[sizeof(BANNED_POPUP_WINDOWS) * 4];
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)&BannedPopupsListBuffer;
    BOOL bRecheckWindows = FALSE;
    HWND hwndOldDesktop = g_DesktopHwnd;
    ULONG uTotalPages, uPage, uDirtyPages = 0;
    RECT rcDirtyArea, rcCurrent;
    BOOL bFirst = TRUE;
    HDC hDC;
    QV_SYNCHRONIZE qvs;

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
        hDC = GetDC(0);
        qvs.uMagic = QVIDEO_MAGIC;
        if (ExtEscape(hDC, QVESC_SYNCHRONIZE, sizeof(qvs), (LPCSTR) &qvs, 0, NULL) <= 0)
            errorf("ExtEscape(synchronize) failed");
        ReleaseDC(0, hDC);

        debugf("DIRTY %d/%d (%d,%d)-(%d,%d)", uDirtyPages, uTotalPages,
            rcDirtyArea.left, rcDirtyArea.top, rcDirtyArea.right, rcDirtyArea.bottom);

        if (uDirtyPages == 0) // nothing changed according to qvideo
            return ERROR_SUCCESS;
    }

    AttachToInputDesktop();
    if (hwndOldDesktop != g_DesktopHwnd)
    {
        bRecheckWindows = TRUE;
        debugf("desktop changed (old 0x%x), refreshing all windows", hwndOldDesktop);
    }

    if (!g_ExplorerHwnd || bRecheckWindows || !IsWindow(g_ExplorerHwnd))
        g_ExplorerHwnd = FindWindow(NULL, L"Program Manager");

    if (!g_TaskbarHwnd || bRecheckWindows || !IsWindow(g_TaskbarHwnd))
    {
        g_TaskbarHwnd = FindWindow(L"Shell_TrayWnd", NULL);

        if (g_TaskbarHwnd)
            if (g_bFullScreenMode)
                ShowWindow(g_TaskbarHwnd, SW_SHOW);
            else
                ShowWindow(g_TaskbarHwnd, SW_HIDE);
    }

    if (!g_StartButtonHwnd || bRecheckWindows || !IsWindow(g_StartButtonHwnd))
    {
        g_StartButtonHwnd = FindWindowEx(g_DesktopHwnd, NULL, L"Button", NULL);

        if (g_StartButtonHwnd)
            if (g_bFullScreenMode)
                ShowWindow(g_StartButtonHwnd, SW_SHOW);
            else
                ShowWindow(g_StartButtonHwnd, SW_HIDE);
    }

    debugf("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopHwnd, g_ExplorerHwnd, g_TaskbarHwnd, g_StartButtonHwnd);

    if (g_bFullScreenMode)
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

    pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC) &g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;

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

    //debugf("success");
    return ERROR_SUCCESS;
}

// Reinitialize everything, called after a session switch.
// This is executed as another thread to avoid g_hShellEventsThread killing itself without finishing the job.
// TODO: use this with session change notification instead of AttachToInputDesktop every time?
// NOTE: this function doesn't close/reopen qvideo's screen section
DWORD WINAPI ResetWatch(PVOID param)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;

    logf("start");

    StopShellEventsThread();

    debugf("removing watches");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList)
    {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;

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
    if (!g_bFullScreenMode)
    {
        StartShellEventsThread();
        ProcessUpdatedWindows(TRUE);
    }

    logf("success");
    return ERROR_SUCCESS;
}

// g_csWatchedWindows critical section must be entered
PWATCHED_DC AddWindowWithInfo(
    HWND hWnd,
    WINDOWINFO *pwi
    )
{
    PWATCHED_DC pWatchedDC = NULL;

    if (!pwi)
        return NULL;

    debugf("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x",
        hWnd, pwi->rcWindow.left, pwi->rcWindow.top, pwi->rcWindow.right, pwi->rcWindow.bottom, pwi->dwStyle, pwi->dwExStyle);

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (pWatchedDC)
        // already being watched
        return pWatchedDC;

    if ((pwi->rcWindow.top - pwi->rcWindow.bottom == 0) || (pwi->rcWindow.right - pwi->rcWindow.left == 0))
        return NULL;

    pWatchedDC = (PWATCHED_DC) malloc(sizeof(WATCHED_DC));
    if (!pWatchedDC)
        return NULL;

    ZeroMemory(pWatchedDC, sizeof(WATCHED_DC));

    pWatchedDC->bVisible = IsWindowVisible(hWnd);

    pWatchedDC->bStyleChecked = FALSE;
    pWatchedDC->uTimeAdded = pWatchedDC->uTimeModalChecked = GetTickCount();

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (pwi->rcWindow.right-pwi->rcWindow.left == g_ScreenWidth
        && pwi->rcWindow.bottom-pwi->rcWindow.top == g_ScreenHeight)
    {
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

    pWatchedDC->hWnd = hWnd;
    pWatchedDC->rcWindow = pwi->rcWindow;

    pWatchedDC->pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));

    pWatchedDC->MaxWidth = g_ScreenWidth;
    pWatchedDC->MaxHeight = g_ScreenHeight;

    if (g_bVchanClientConnected)
    {
        send_window_create(pWatchedDC);
        send_wmname(hWnd);
    }

    InsertTailList(&g_WatchedWindowsList, &pWatchedDC->le);

    //	_tprintf(_T("created: %x, left: %d top: %d right: %d bottom %d\n"), hWnd, pWatchedDC->rcWindow.left, pWatchedDC->rcWindow.top,
    //		 pWatchedDC->rcWindow.right, pWatchedDC->rcWindow.bottom);

    //debugf("success");
    return pWatchedDC;
}

PWATCHED_DC AddWindow(HWND hWnd)
{
    PWATCHED_DC pWatchedDC = NULL;
    WINDOWINFO wi;

    debugf("0x%x", hWnd);
    if (hWnd == 0)
        return NULL;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
    {
        perror("GetWindowInfo");
        return NULL;
    }

    if (!ShouldAcceptWindow(hWnd, &wi))
        return NULL;

    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = AddWindowWithInfo(hWnd, &wi);

    LeaveCriticalSection(&g_csWatchedWindows);

    //debugf("success");
    return pWatchedDC;
}

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC)
{
    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    debugf("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);
    free(pWatchedDC->pPfnArray);

    if (g_bVchanClientConnected)
    {
        send_window_unmap(pWatchedDC->hWnd);
        if (pWatchedDC->hWnd) // never destroy screen "window"
            send_window_destroy(pWatchedDC->hWnd);
    }

    free(pWatchedDC);

    return ERROR_SUCCESS;
}

ULONG RemoveWindow(HWND hWnd)
{
    PWATCHED_DC pWatchedDC;

    debugf("0x%x", hWnd);
    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (!pWatchedDC)
    {
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    RemoveEntryList(&pWatchedDC->le);
    RemoveWatchedDC(pWatchedDC);
    pWatchedDC = NULL;

    LeaveCriticalSection(&g_csWatchedWindows);

    //debugf("success");
    return ERROR_SUCCESS;
}

// called from shell hook proc
ULONG CheckWindowUpdates(HWND hWnd)
{
    WINDOWINFO wi;
    PWATCHED_DC pWatchedDC = NULL;

    debugf("0x%x", hWnd);
    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return perror("GetWindowInfo");

    EnterCriticalSection(&g_csWatchedWindows);

    // AddWindowWithRect() returns an existing pWatchedDC if the window is already on the list.
    pWatchedDC = AddWindowWithInfo(hWnd, &wi);
    if (!pWatchedDC)
    {
        logf("AddWindowWithInfo returned NULL");
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    CheckWatchedWindowUpdates(pWatchedDC, &wi, FALSE, NULL);

    LeaveCriticalSection(&g_csWatchedWindows);

    send_wmname(hWnd);

    //debugf("success");
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
            debugf("HSHELL_REDRAW");
            goto update;
        case HSHELL_RUDEAPPACTIVATED:
            debugf("HSHELL_RUDEAPPACTIVATED");
            goto update;
        case HSHELL_WINDOWACTIVATED:
            debugf("HSHELL_RUDEAPPACTIVATED");
            goto update;
        case HSHELL_GETMINRECT:
            debugf("HSHELL_GETMINRECT");
            targetWindow = ((SHELLHOOKINFO*)lParam)->hwnd;
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
        logf("session change: event 0x%x, session %d", wParam, lParam);
        //if (!CreateThread(0, 0, ResetWatch, NULL, 0, NULL))
        //    perror("CreateThread(ResetWatch)");
        break;
    case WM_CLOSE:
        debugf("WM_CLOSE");
        //if (!WTSUnRegisterSessionNotification(hwnd))
        //    perror("WTSUnRegisterSessionNotification");
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        debugf("WM_DESTROY");
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

ULONG CreateShellHookWindow(HWND *pHwnd)
{
    WNDCLASSEX wc;
    HWND hwnd;
    HINSTANCE hInstance = GetModuleHandle(NULL);
    ULONG uResult;

    debugf("start");
    if (!pHwnd)
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
    logf("shell hook window: 0x%x", hwnd);

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

    *pHwnd = hwnd;

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG HookMessageLoop()
{
    MSG Msg;
    HINSTANCE hInstance = GetModuleHandle(NULL);

    debugf("start");
    while (GetMessage(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    debugf("exiting");
    return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(PVOID pParam)
{
    ULONG uResult;

    logf("shell events thread started");
    if (ERROR_SUCCESS != AttachToInputDesktop())
        return perror("AttachToInputDesktop");

    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWnd))
        return perror("CreateShellHookWindow");

    InvalidateRect(NULL, NULL, TRUE); // repaint everything
    if (ERROR_SUCCESS != (uResult = HookMessageLoop()))
        return uResult;

    if (!UnregisterClass(g_szClassName, NULL))
        return perror("UnregisterClass");

    return ERROR_SUCCESS;
}

ULONG StartShellEventsThread()
{
    DWORD threadId;

    debugf("start");

    if (!g_Initialized)
    {
        InitializeListHead(&g_WatchedWindowsList);
        InitializeCriticalSection(&g_csWatchedWindows);

        g_Initialized = TRUE;
    }

    g_hShellEventsThread = CreateThread(NULL, 0, ShellEventsThread, NULL, 0, &threadId);
    if (!g_hShellEventsThread)
        return perror("CreateThread(ShellEventsThread)");

    logf("shell events thread ID: %d (created)", threadId);
    return ERROR_SUCCESS;
}

ULONG StopShellEventsThread()
{
    logf("shell hook window: 0x%x", g_ShellEventsWnd);
    if (!g_hShellEventsThread)
        return ERROR_SUCCESS;

    if (!PostMessage(g_ShellEventsWnd, WM_CLOSE, 0, 0))
        return perror("PostMessage(WM_CLOSE)");

    debugf("waiting for thread to exit");
    // FIXME: timeout
    WaitForSingleObject(g_hShellEventsThread, INFINITE);

    CloseHandle(g_hShellEventsThread);

    g_hShellEventsThread = NULL;

    logf("shell events thread terminated");
    return ERROR_SUCCESS;
}
