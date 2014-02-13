#include "shell_events.h"
#include "log.h"

const TCHAR g_szClassName[] = _T("QubesShellHookClass");
ULONG g_uShellHookMessage = 0;
HWND g_ShellEventsWnd = NULL;
HANDLE g_hShellEventsThread = NULL;

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;
BOOLEAN g_Initialized = FALSE;

LONG g_ScreenHeight = 0;
LONG g_ScreenWidth = 0;
PUCHAR g_pScreenData = NULL;
HANDLE g_hSection;

HWND g_DesktopHwnd = NULL;
HWND g_ExplorerHwnd = NULL;
HWND g_TaskbarHwnd = NULL;
HWND g_StartButtonHwnd = NULL;

extern BOOLEAN g_bVchanClientConnected;

PWATCHED_DC AddWindowWithInfo(
    HWND hWnd,
    WINDOWINFO *pwi
);

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC);

ULONG OpenScreenSection()
{
    TCHAR SectionName[100];

    // already initialized
    if (g_hSection && g_pScreenData)
        return ERROR_SUCCESS;

    StringCchPrintf(SectionName, _countof(SectionName),
        _T("Global\\QubesSharedMemory_%x"), g_ScreenHeight * g_ScreenWidth * 4);
    debugf("%s", SectionName);

    g_hSection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
    if (!g_hSection) {
        return perror("OpenFileMapping");
    }

    g_pScreenData = MapViewOfFile(g_hSection, FILE_MAP_READ, 0, 0, 0);
    if (!g_pScreenData) {
        return perror("MapViewOfFile");
    }

    debugf("success: section=0x%x, data=0x%x", g_hSection, g_pScreenData);
    return ERROR_SUCCESS;
}

PWATCHED_DC FindWindowByHwnd(HWND hWnd)
{
    PWATCHED_DC pWatchedDC;

    debugf("%x", hWnd);
    pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList) {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);

        if (hWnd == pWatchedDC->hWnd)
            return pWatchedDC;

        pWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;
    }

    return NULL;
}

#define ENFORCE_LIMITS(var, min, max) \
    if (var < min) \
        var = min; \
    if (var > max) \
        var = max;

ULONG SanitizeRect(
    RECT *pRect,
    LONG MaxHeight,
    LONG MaxWidth
)
{
    if (!pRect)
        return ERROR_INVALID_PARAMETER;

    ENFORCE_LIMITS(pRect->right, 0, MaxWidth);
    ENFORCE_LIMITS(pRect->bottom, 0, MaxHeight);

    ENFORCE_LIMITS(pRect->left, 0, pRect->right);
    ENFORCE_LIMITS(pRect->top, 0, pRect->bottom);

    return ERROR_SUCCESS;
}

// pRect must be sanitized
ULONG CopyScreenData(
    BYTE *pCompositionBuffer,
    RECT *pRect
)
{
    LONG Line;
    ULONG uWindowStride;
    ULONG uScreenStride;
    PUCHAR pSourceLine = NULL;
    PUCHAR pDestLine = NULL;

    if (!pCompositionBuffer || !g_pScreenData || !pRect)
        return ERROR_INVALID_PARAMETER;

    debugf("buffer=%p, (%d,%d)-(%d,%d)", pCompositionBuffer, pRect->left, pRect->top, pRect->right, pRect->bottom);

    uWindowStride = (pRect->right - pRect->left) * 4;
    uScreenStride = g_ScreenWidth * 4;

    pDestLine = pCompositionBuffer;
    pSourceLine = g_pScreenData + (uScreenStride * pRect->top) + pRect->left * 4;

    for (Line = pRect->top; Line < pRect->bottom; Line++) {
        memcpy(pDestLine, pSourceLine, uWindowStride);

        pDestLine += uWindowStride;
        pSourceLine += uScreenStride;
    }

    return ERROR_SUCCESS;
}

ULONG CheckWatchedWindowUpdates(
    PWATCHED_DC pWatchedDC,
    WINDOWINFO *pwi,
    BOOLEAN bDamageDetected
)
{
    WINDOWINFO wi;
    BOOLEAN	bResizingDetected;
    BOOLEAN bMoveDetected;
    BOOL	bCurrentlyVisible;
    BOOL	bUpdateStyle;

    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    debugf("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);

    if (!pwi) {
        wi.cbSize = sizeof(wi);
        if (!GetWindowInfo(pWatchedDC->hWnd, &wi)) {
            return perror("GetWindowInfo");
        }
    } else
        memcpy(&wi, pwi, sizeof(wi));

    bCurrentlyVisible = IsWindowVisible(pWatchedDC->hWnd);
    if (g_bVchanClientConnected) {
        if (bCurrentlyVisible && !pWatchedDC->bVisible)
            send_window_map(pWatchedDC);

        if (!bCurrentlyVisible && pWatchedDC->bVisible)
            send_window_unmap(pWatchedDC->hWnd);
    }

    if (!pWatchedDC->bStyleChecked && (GetTickCount() >= pWatchedDC->uTimeAdded + 500)) {
        pWatchedDC->bStyleChecked = TRUE;

        bUpdateStyle = FALSE;
        if (wi.dwStyle & WS_MINIMIZEBOX) {
            wi.dwStyle &= ~WS_MINIMIZEBOX;
            bUpdateStyle = TRUE;
            DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
        }
/*		if (Style & WS_MAXIMIZEBOX) {
            Style &= ~WS_MAXIMIZEBOX;
            bUpdateStyle = TRUE;
            DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MAXIMIZE, MF_BYCOMMAND);
        }
*/
        if (wi.dwStyle & WS_SIZEBOX) {
            wi.dwStyle &= ~WS_SIZEBOX;
            bUpdateStyle = TRUE;
        }

        if (bUpdateStyle) {
            SetWindowLong(pWatchedDC->hWnd, GWL_STYLE, wi.dwStyle);
            DrawMenuBar(pWatchedDC->hWnd);
        }
    }

    pWatchedDC->bVisible = bCurrentlyVisible;

    SanitizeRect(&wi.rcWindow, pWatchedDC->MaxHeight, pWatchedDC->MaxWidth);

    bMoveDetected = wi.rcWindow.left != pWatchedDC->rcWindow.left ||
        wi.rcWindow.top != pWatchedDC->rcWindow.top ||
        wi.rcWindow.right != pWatchedDC->rcWindow.right || wi.rcWindow.bottom != pWatchedDC->rcWindow.bottom;

    bDamageDetected |= bMoveDetected;

    bResizingDetected = (wi.rcWindow.right - wi.rcWindow.left != pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left) ||
        (wi.rcWindow.bottom - wi.rcWindow.top != pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);

    if (bDamageDetected || bResizingDetected) {
//		_tprintf(_T("hwnd: %x, left: %d top: %d right: %d bottom %d\n"), pWatchedDC->hWnd, wi.rcWindow.left, wi.rcWindow.top, wi.rcWindow.right,
//			 wi.rcWindow.bottom);

        pWatchedDC->rcWindow = wi.rcWindow;
        CopyScreenData(pWatchedDC->pCompositionBuffer, &pWatchedDC->rcWindow);

        if (g_bVchanClientConnected) {
            if (bMoveDetected || bResizingDetected)
                send_window_configure(pWatchedDC);

            if (bResizingDetected)
                send_pixmap_mfns(pWatchedDC);

            send_window_damage_event(pWatchedDC->hWnd, 0, 0,
                pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left,
                pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);
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
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)lParam;
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

    if (!desktop) {
        uResult = perror("OpenInputDesktop");
        goto cleanup;
    }

#ifdef DEBUG
    if (!GetUserObjectInformation(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        perror("GetUserObjectInformation");
    }
    else {
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
    if (!SetThreadDesktop(desktop)) {
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

// Main function that scans for window updates.
// Runs in the main thread.
ULONG ProcessUpdatedWindows(BOOLEAN bUpdateEverything)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;
    CHAR BannedPopupsListBuffer[sizeof(BANNED_POPUP_WINDOWS) * 4];
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)&BannedPopupsListBuffer;
    BOOLEAN bRecheckWindows = FALSE;
    HWND oldDesktop = g_DesktopHwnd;

    debugf("update all? %d", bUpdateEverything);

    AttachToInputDesktop();
    if (oldDesktop != g_DesktopHwnd) {
        bRecheckWindows = TRUE;
        debugf("desktop changed (old 0x%x), refreshing all windows", oldDesktop);
    }

    if (!g_ExplorerHwnd || bRecheckWindows)
        g_ExplorerHwnd = FindWindow(NULL, _T("Program Manager"));

    if (!g_TaskbarHwnd || bRecheckWindows) {
        g_TaskbarHwnd = FindWindow(_T("Shell_TrayWnd"), NULL);

        if (g_TaskbarHwnd)
            ShowWindow(g_TaskbarHwnd, SW_HIDE);
    }

    if (!g_StartButtonHwnd || bRecheckWindows) {
        g_StartButtonHwnd = FindWindowEx(g_DesktopHwnd, NULL, _T("Button"), NULL);

        if (g_StartButtonHwnd)
            ShowWindow(g_StartButtonHwnd, SW_HIDE);
    }

    debugf("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopHwnd, g_ExplorerHwnd, g_TaskbarHwnd, g_StartButtonHwnd);

    pBannedPopupsList->uNumberOfBannedPopups = 4;
    pBannedPopupsList->hBannedPopupArray[0] = g_DesktopHwnd;
    pBannedPopupsList->hBannedPopupArray[1] = g_ExplorerHwnd;
    pBannedPopupsList->hBannedPopupArray[2] = g_TaskbarHwnd;
    pBannedPopupsList->hBannedPopupArray[3] = g_StartButtonHwnd;

    EnterCriticalSection(&g_csWatchedWindows);

    EnumWindows(EnumWindowsProc, (LPARAM)pBannedPopupsList);

    pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
    while (pWatchedDC != (PWATCHED_DC) &g_WatchedWindowsList) {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;

        if (!IsWindow(pWatchedDC->hWnd)) {
            RemoveEntryList(&pWatchedDC->le);
            RemoveWatchedDC(pWatchedDC);
            pWatchedDC = NULL;
        } else
            CheckWatchedWindowUpdates(pWatchedDC, NULL, bUpdateEverything);

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
    while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList) {
        pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);
        pNextWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;

        RemoveEntryList(&pWatchedDC->le);
        RemoveWatchedDC(pWatchedDC);

        pWatchedDC = pNextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    // todo: wait for desktop switch - it can take some time after the session event

    StartShellEventsThread();

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

    debugf("0x%x", hWnd);
    if (!pwi)
        return NULL;

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (pWatchedDC)
        // already being watched
        return pWatchedDC;

    SanitizeRect(&pwi->rcWindow, g_ScreenHeight, g_ScreenWidth);

    if ((pwi->rcWindow.top - pwi->rcWindow.bottom == 0) || (pwi->rcWindow.right - pwi->rcWindow.left == 0))
        return NULL;

    pWatchedDC = malloc(sizeof(WATCHED_DC));
    if (!pWatchedDC)
        return NULL;

    ZeroMemory(pWatchedDC, sizeof(WATCHED_DC));

    pWatchedDC->bVisible = IsWindowVisible(hWnd);

    pWatchedDC->bStyleChecked = FALSE;
    pWatchedDC->uTimeAdded = GetTickCount();

    // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (pwi->rcWindow.right-pwi->rcWindow.left == g_ScreenWidth && pwi->rcWindow.bottom-pwi->rcWindow.top == g_ScreenHeight)
        pWatchedDC->bOverrideRedirect = FALSE;
    else
        pWatchedDC->bOverrideRedirect = (BOOL)((WS_CAPTION & pwi->dwStyle) != WS_CAPTION);

    pWatchedDC->hWnd = hWnd;
    pWatchedDC->rcWindow = pwi->rcWindow;

    pWatchedDC->MaxHeight = g_ScreenHeight;
    pWatchedDC->MaxWidth = g_ScreenWidth;

    pWatchedDC->uCompositionBufferSize = pWatchedDC->MaxHeight * pWatchedDC->MaxWidth * 4;

    pWatchedDC->pCompositionBuffer = VirtualAlloc(NULL, pWatchedDC->uCompositionBufferSize, MEM_COMMIT, PAGE_READWRITE);
    if (!pWatchedDC->pCompositionBuffer) {
        perror("VirtualAlloc");
        //perror("VirtualAlloc(%d)", pWatchedDC->uCompositionBufferSize);
        free(pWatchedDC);
        return NULL;
    }

    if (!VirtualLock(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize)) {
        perror("VirtualLock");
        VirtualFree(pWatchedDC->pCompositionBuffer, 0, MEM_RELEASE);
        free(pWatchedDC);
        return NULL;
    }

    if (ERROR_SUCCESS != GetPfnList(pWatchedDC->pCompositionBuffer,
        pWatchedDC->uCompositionBufferSize, &pWatchedDC->PfnArray)) {
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

    if (g_bVchanClientConnected) {
        send_window_create(pWatchedDC);
        send_pixmap_mfns(pWatchedDC);
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
    if (!GetWindowInfo(hWnd, &wi)) {
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
    if (g_bVchanClientConnected) {
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
    if (!pWatchedDC) {
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
    if (!pWatchedDC) {
        logf("AddWindowWithInfo returned NULL");
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    CheckWatchedWindowUpdates(pWatchedDC, &wi, FALSE);

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

    if (uMsg == g_uShellHookMessage) {
        switch (wParam) {
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

    switch (uMsg) {
    case WM_WTSSESSION_CHANGE:
        logf("session change: event 0x%x, session %d", wParam, lParam);
        //if (!CreateThread(0, 0, ResetWatch, NULL, 0, NULL)) {
        //    perror("CreateThread(ResetWatch)");
        //}
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
//      _tprintf(_T("hwnd: %x, msg %d\n"), hwnd, uMsg);
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

    if (!RegisterClassEx(&wc)) {
        return perror("RegisterClassEx");
    }

    hwnd = CreateWindow(g_szClassName, _T("QubesShellHook"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) {
        return perror("CreateWindow");
    }

    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    if (!RegisterShellHookWindow(hwnd)) {
        uResult = perror("RegisterShellHookWindow");
        DestroyWindow(hwnd);
        UnregisterClass(g_szClassName, hInstance);
        return uResult;
    }
    /*
    if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_ALL_SESSIONS)) {
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
    while (GetMessage(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    debugf("exiting");
    return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(PVOID pParam)
{
    ULONG uResult;

    if (ERROR_SUCCESS != AttachToInputDesktop()) {
        return perror("AttachToInputDesktop");
    }

    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWnd)) {
        return perror("CreateShellHookWindow");
    }

    if (ERROR_SUCCESS != (uResult = HookMessageLoop())) {
        return uResult;
    }

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
         if (ERROR_SUCCESS != OpenScreenSection()) {
            return perror("OpenScreenSection");
        }

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
    if (!g_hShellEventsThread) {
        return perror("CreateThread(ShellEventsThread)");
    }

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
