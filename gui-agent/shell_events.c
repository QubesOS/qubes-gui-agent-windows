#include "shell_events.h"
#include "log.h"

//#define PER_WINDOW_BUFFER 1

const TCHAR g_szClassName[] = _T("QubesShellHookClass");
ULONG g_uShellHookMessage = 0;
HWND g_ShellEventsWnd = NULL;
HANDLE g_hShellEventsThread = NULL;

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;
BOOL g_Initialized = FALSE;

LONG g_ScreenHeight = 0;
LONG g_ScreenWidth = 0;
PBYTE g_pScreenData = NULL;
HANDLE g_hSection = NULL;

// bit array of dirty pages in the screen buffer (changed since last check)
PQV_DIRTY_PAGES g_pDirtyPages = NULL;
HANDLE g_hDirtySection = NULL;

HWND g_DesktopHwnd = NULL;
HWND g_ExplorerHwnd = NULL;
HWND g_TaskbarHwnd = NULL;
HWND g_StartButtonHwnd = NULL;

extern BOOL g_bVchanClientConnected;
extern BOOL g_bFullScreenMode;

PWATCHED_DC AddWindowWithInfo(
    HWND hWnd,
    WINDOWINFO *pwi
    );

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC);

ULONG OpenScreenSection()
{
    TCHAR SectionName[100];
    ULONG uLength = g_ScreenHeight * g_ScreenWidth * 4;

    // already initialized
    if (g_hSection && g_pScreenData)
        return ERROR_SUCCESS;

    StringCchPrintf(SectionName, _countof(SectionName),
        _T("Global\\QubesSharedMemory_%x"), uLength);
    debugf("screen section: %s", SectionName);

    g_hSection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
    if (!g_hSection)
        return perror("OpenFileMapping(screen section)");

    g_pScreenData = (PBYTE) MapViewOfFile(g_hSection, FILE_MAP_READ, 0, 0, 0);
    if (!g_pScreenData)
        return perror("MapViewOfFile(screen section)");

    uLength /= PAGE_SIZE;
    StringCchPrintf(SectionName, _countof(SectionName),
        _T("Global\\QvideoDirtyPages_%x"), sizeof(QV_DIRTY_PAGES) + (uLength >> 3) + 1);
    debugf("dirty section: %s", SectionName);

    g_hDirtySection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
    if (!g_hDirtySection)
        return perror("OpenFileMapping(dirty section)");

    g_pDirtyPages = (PQV_DIRTY_PAGES) MapViewOfFile(g_hDirtySection, FILE_MAP_READ, 0, 0, 0);
    if (!g_pDirtyPages)
        return perror("MapViewOfFile(dirty section)");

    debugf("success: dirty section=0x%x, data=0x%x", g_hDirtySection, g_pDirtyPages);
    return ERROR_SUCCESS;
}

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

ULONG CopyScreenData(
    BYTE *pCompositionBuffer,
    RECT *pRect
    )
{
    LONG Line;
    ULONG uWindowStride;
    ULONG uScreenStride;
    PBYTE pSourceLine = NULL;
    PBYTE pDestLine = NULL;
    PBYTE pScreenBufferLimit;
    //ULONG diff = 0;

    if (!pCompositionBuffer || !g_pScreenData || !pRect)
        return ERROR_INVALID_PARAMETER;

    uWindowStride = (pRect->right - pRect->left) * 4;
    uScreenStride = g_ScreenWidth * 4;
    // Limit of screen buffer memory.
    pScreenBufferLimit = g_pScreenData + uScreenStride*g_ScreenHeight;

    // Range checking done manually later.
    //SanitizeRect(pRect, g_ScreenHeight, g_ScreenWidth);
    // FIXME: limit window width to screen width or handle that below

    debugf("buffer=%p, (%d,%d)-(%d,%d), win stride %d, screen stride %d",
        pCompositionBuffer, pRect->left, pRect->top, pRect->right, pRect->bottom,
        uWindowStride, uScreenStride);

    if (pRect->bottom - pRect->top == 0)
        return ERROR_SUCCESS;

    pDestLine = pCompositionBuffer;
    pSourceLine = g_pScreenData + (uScreenStride * pRect->top) + pRect->left * 4;

    for (Line = pRect->top; Line < pRect->bottom; Line++)
    {
        if (pSourceLine >= g_pScreenData)
        {
            if (pSourceLine+uScreenStride >= pScreenBufferLimit)
                break; // end of screen buffer reached
            //if (memcmp(pDestLine, pSourceLine, uWindowStride))
            //    diff++;
            memcpy(pDestLine, pSourceLine, uWindowStride);
        }
        pDestLine += uWindowStride;
        pSourceLine += uScreenStride;
    }
    //debugf("%d/%d lines differ (%d%%)", diff, pRect->bottom-pRect->top, 100*diff/(pRect->bottom-pRect->top));

    return ERROR_SUCCESS;
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
        /*		if (Style & WS_MAXIMIZEBOX)
        {
        Style &= ~WS_MAXIMIZEBOX;
        bUpdateStyle = TRUE;
        DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MAXIMIZE, MF_BYCOMMAND);
        }
        */
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

    pWatchedDC->bVisible = bCurrentlyVisible;

    bMoveDetected = wi.rcWindow.left != pWatchedDC->rcWindow.left ||
        wi.rcWindow.top != pWatchedDC->rcWindow.top ||
        wi.rcWindow.right != pWatchedDC->rcWindow.right || wi.rcWindow.bottom != pWatchedDC->rcWindow.bottom;

    bDamageDetected |= bMoveDetected;

    bResizingDetected = (wi.rcWindow.right - wi.rcWindow.left != pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left) ||
        (wi.rcWindow.bottom - wi.rcWindow.top != pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);

    if (bDamageDetected || bResizingDetected)
    {
        //		_tprintf(_T("hwnd: %x, left: %d top: %d right: %d bottom %d\n"), pWatchedDC->hWnd, wi.rcWindow.left, wi.rcWindow.top, wi.rcWindow.right,
        //			 wi.rcWindow.bottom);
        pWatchedDC->rcWindow = wi.rcWindow;

#if PER_WINDOW_BUFFER
        CopyScreenData(pWatchedDC->pCompositionBuffer, &pWatchedDC->rcWindow);
#endif

        if (g_bVchanClientConnected)
        {
            RECT intersection;

            if (bMoveDetected || bResizingDetected)
                send_window_configure(pWatchedDC);

#if PER_WINDOW_BUFFER
            if (bResizingDetected)
                send_pixmap_mfns(pWatchedDC);
#endif

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

BOOL ShouldAcceptWindow(HWND hWnd, WINDOWINFO *pwi)
{
    //debugf("0x%x", hWnd);
    if (!IsWindowVisible(hWnd))
        return FALSE;

    // If a window has a parent window, has no caption and is not a tool window,
    // then don't show it as a separate window.
    if (GetParent(hWnd) && ((WS_CAPTION & pwi->dwStyle) != WS_CAPTION) && !(WS_EX_TOOLWINDOW & pwi->dwExStyle))
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
    TCHAR name[256];
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
    CHAR BannedPopupsListBuffer[sizeof(BANNED_POPUP_WINDOWS) * 4];
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)&BannedPopupsListBuffer;
    BOOL bRecheckWindows = FALSE;
    HWND hwndOldDesktop = g_DesktopHwnd;
    ULONG uTotalPages, uPage, uDirtyPages = 0;
    RECT rcDirtyArea, rcCurrent;
    BOOL bFirst = TRUE;
    HDC hDC;
    QV_GET_SURFACE_DATA qvgsd;

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
    qvgsd.uMagic = QVIDEO_MAGIC;
    if (ExtEscape(hDC, QVESC_SYNCHRONIZE, sizeof(QVESC_GET_SURFACE_DATA), (LPCSTR) &qvgsd, 0, NULL) <= 0)
        errorf("ExtEscape(ready) failed");
    ReleaseDC(0, hDC);

    debugf("DIRTY %d/%d (%d,%d)-(%d,%d)", uDirtyPages, uTotalPages,
        rcDirtyArea.left, rcDirtyArea.top, rcDirtyArea.right, rcDirtyArea.bottom);

    if (uDirtyPages == 0) // nothing changed according to qvideo
        return ERROR_SUCCESS;

    AttachToInputDesktop();
    if (hwndOldDesktop != g_DesktopHwnd)
    {
        bRecheckWindows = TRUE;
        debugf("desktop changed (old 0x%x), refreshing all windows", hwndOldDesktop);
    }

    if (!g_ExplorerHwnd || bRecheckWindows)
        g_ExplorerHwnd = FindWindow(NULL, _T("Program Manager"));

    if (!g_TaskbarHwnd || bRecheckWindows)
    {
        g_TaskbarHwnd = FindWindow(_T("Shell_TrayWnd"), NULL);

        if (g_TaskbarHwnd)
            if (g_bFullScreenMode)
                ShowWindow(g_TaskbarHwnd, SW_SHOW);
            else
                ShowWindow(g_TaskbarHwnd, SW_HIDE);
    }

    if (!g_StartButtonHwnd || bRecheckWindows)
    {
        g_StartButtonHwnd = FindWindowEx(g_DesktopHwnd, NULL, _T("Button"), NULL);

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
        send_window_damage_event(0, rcDirtyArea.left, rcDirtyArea.top,
            rcDirtyArea.right - rcDirtyArea.left,
            rcDirtyArea.bottom - rcDirtyArea.top);
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

        if (!IsWindow(pWatchedDC->hWnd))
        {
            RemoveEntryList(&pWatchedDC->le);
            RemoveWatchedDC(pWatchedDC);
            pWatchedDC = NULL;
        }
        else
        {
            if (IntersectRect(&rcCurrent, &rcDirtyArea, &pWatchedDC->rcWindow))
                // skip windows that aren't in the changed area
                CheckWatchedWindowUpdates(pWatchedDC, NULL, bUpdateEverything, &rcDirtyArea);
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
DWORD WINAPI ResetWatch(PVOID param)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;

    debugf("start");

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

    // don't start shell events thread if we're in fullscreen mode
    // WatchForEvents will map the whole screen as one window
    if (!g_bFullScreenMode)
    {
        StartShellEventsThread();
    }

    //debugf("success");
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

    debugf("0x%x (%d,%d)-(%d,%d)",
        hWnd, pwi->rcWindow.left, pwi->rcWindow.top, pwi->rcWindow.right, pwi->rcWindow.bottom);

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (pWatchedDC)
        // already being watched
            return pWatchedDC;

    //SanitizeRect(&pwi->rcWindow, g_ScreenHeight, g_ScreenWidth);

    if ((pwi->rcWindow.top - pwi->rcWindow.bottom == 0) || (pwi->rcWindow.right - pwi->rcWindow.left == 0))
        return NULL;

    pWatchedDC = (PWATCHED_DC) malloc(sizeof(WATCHED_DC));
    if (!pWatchedDC)
        return NULL;

    ZeroMemory(pWatchedDC, sizeof(WATCHED_DC));

    pWatchedDC->bVisible = IsWindowVisible(hWnd);

    pWatchedDC->bStyleChecked = FALSE;
    pWatchedDC->uTimeAdded = GetTickCount();

    // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (pwi->rcWindow.right-pwi->rcWindow.left == g_ScreenWidth
        && pwi->rcWindow.bottom-pwi->rcWindow.top == g_ScreenHeight)
    {
        pWatchedDC->bOverrideRedirect = FALSE;
    }
    else
        pWatchedDC->bOverrideRedirect = (BOOL) ((WS_CAPTION & pwi->dwStyle) != WS_CAPTION);

    pWatchedDC->hWnd = hWnd;
    pWatchedDC->rcWindow = pwi->rcWindow;

    pWatchedDC->MaxHeight = g_ScreenHeight;
    pWatchedDC->MaxWidth = g_ScreenWidth;

#if PER_WINDOW_BUFFER
    pWatchedDC->uCompositionBufferSize = pWatchedDC->MaxHeight * pWatchedDC->MaxWidth * 4;

    pWatchedDC->pCompositionBuffer =
        (PUCHAR) VirtualAlloc(NULL, pWatchedDC->uCompositionBufferSize, MEM_COMMIT, PAGE_READWRITE);

    if (!pWatchedDC->pCompositionBuffer)
    {
        perror("VirtualAlloc");
        //perror("VirtualAlloc(%d)", pWatchedDC->uCompositionBufferSize);
        free(pWatchedDC);
        return NULL;
    }

    if (!VirtualLock(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize))
    {
        perror("VirtualLock");
        VirtualFree(pWatchedDC->pCompositionBuffer, 0, MEM_RELEASE);
        free(pWatchedDC);
        return NULL;
    }

    if (ERROR_SUCCESS != GetPfnList(pWatchedDC->pCompositionBuffer,
        pWatchedDC->uCompositionBufferSize, &pWatchedDC->PfnArray))
    {
            perror("GetPfnList");
            VirtualUnlock(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize);
            VirtualFree(pWatchedDC->pCompositionBuffer, 0, MEM_RELEASE);
            free(pWatchedDC);
            return NULL;
    }
    /*
    uResult = AllocateAndMapPhysicalMemory((pWatchedDC->uCompositionBufferSize + 0xfff) >> 12, &pWatchedDC->pCompositionBuffer, &pWatchedDC->PfnArray);
    if (ERROR_SUCCESS != uResult) {
    _tprintf(_T(__FUNCTION__) _T("(): AllocateAndMapPhysicalMemory() failed, error %d\n"), uResult);
    free(pWatchedDC);
    return NULL;
    }
    */

    logf("created: %x, pages: %d: %d %d %d\n",
        hWnd, pWatchedDC->PfnArray.uNumberOf4kPages,
        pWatchedDC->PfnArray.Pfn[0], pWatchedDC->PfnArray.Pfn[1], pWatchedDC->PfnArray.Pfn[2]);

    CopyScreenData(pWatchedDC->pCompositionBuffer, &pWatchedDC->rcWindow);
#endif

    if (g_bVchanClientConnected)
    {
        send_window_create(pWatchedDC);
#if PER_WINDOW_BUFFER
        send_pixmap_mfns(pWatchedDC);
#endif
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
    VirtualUnlock(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize);
    VirtualFree(pWatchedDC->pCompositionBuffer, 0, MEM_RELEASE);

    /*
    uResult = UnmapAndFreePhysicalMemory(&pWatchedDC->pCompositionBuffer, &pWatchedDC->PfnArray);
    if (ERROR_SUCCESS != uResult)
    _tprintf(_T(__FUNCTION__) _T("(): UnmapAndFreePhysicalMemory() failed, error %d\n"), uResult);

    */
    if (g_bVchanClientConnected)
    {
        send_window_unmap(pWatchedDC->hWnd);
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
        //_tprintf(_T("hwnd: %x, msg %d\n"), hwnd, uMsg);
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

    hwnd = CreateWindow(g_szClassName, _T("QubesShellHook"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInstance, NULL);

    if (hwnd == NULL)
        return perror("CreateWindow");

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
        g_uShellHookMessage = RegisterWindowMessage(_T("SHELLHOOK"));

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

    if (ERROR_SUCCESS != AttachToInputDesktop())
        return perror("AttachToInputDesktop");

    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWnd))
        return perror("CreateShellHookWindow");

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
        if (ERROR_SUCCESS != OpenScreenSection())
            return perror("OpenScreenSection");

        /*
        if (!LoggedSetLockPagesPrivilege(TRUE)) {
        _tprintf(_T(__FUNCTION__) _T("(): LoggedSetLockPagesPrivilege() failed\n"));
        return ERROR_PRIVILEGE_NOT_HELD;
        }
        */
        InitializeListHead(&g_WatchedWindowsList);
        InitializeCriticalSection(&g_csWatchedWindows);

        g_Initialized = TRUE;
    }

    g_hShellEventsThread = CreateThread(NULL, 0, ShellEventsThread, NULL, 0, &threadId);
    if (!g_hShellEventsThread)
        return perror("CreateThread(ShellEventsThread)");

    debugf("new thread ID: %d", threadId);
    return ERROR_SUCCESS;
}

ULONG StopShellEventsThread()
{
    debugf("start");
    if (!g_hShellEventsThread)
        return ERROR_SUCCESS;

    if (!PostMessage(g_ShellEventsWnd, WM_CLOSE, 0, 0))
        perror("PostMessage(WM_CLOSE)");

    debugf("waiting for thread to exit");
    WaitForSingleObject(g_hShellEventsThread, INFINITE);

    CloseHandle(g_hShellEventsThread);

    g_hShellEventsThread = NULL;

    //debugf("success");
    return ERROR_SUCCESS;
}
