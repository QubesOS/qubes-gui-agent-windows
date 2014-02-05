#include "shell_events.h"
#include "log.h"

const TCHAR g_szClassName[] = _T("QubesShellHookClass");
ULONG g_uShellHookMessage = 0;
HWND g_ShellEventsWnd = NULL;
HANDLE g_hShellEventsThread = NULL;

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

LONG g_ScreenHeight = 0;
LONG g_ScreenWidth = 0;
PUCHAR g_pScreenData = NULL;
HANDLE g_hSection;

HWND g_DesktopHwnd = NULL;
HWND g_ExplorerHwnd = NULL;
HWND g_TaskbarHwnd = NULL;
HWND g_StartButtonHwnd = NULL;

extern BOOLEAN g_bVchanClientConnected;

PWATCHED_DC AddWindowWithRect(
    HWND hWnd,
    RECT *pRect
);

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC);

ULONG OpenScreenSection(
)
{
    TCHAR SectionName[100];

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

    debugf("success");
    return ERROR_SUCCESS;
}

PWATCHED_DC FindWindowByHwnd(
    HWND hWnd
)
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
    RECT *pRect,
    BOOLEAN bDamageDetected
)
{
    WINDOWINFO wi;
    BOOLEAN	bResizingDetected;
    BOOLEAN bMoveDetected;
    BOOL	bCurrentlyVisible;
    LONG	Style;
    BOOL	bUpdateStyle;

    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    debugf("hwnd=0x%x, hdc=0x%x", pWatchedDC->hWnd, pWatchedDC->hDC);

    if (!pRect) {
        wi.cbSize = sizeof(wi);
        if (!GetWindowInfo(pWatchedDC->hWnd, &wi)) {
            return perror("GetWindowInfo");
        }
    } else
        wi.rcWindow = *pRect;

    bCurrentlyVisible = IsWindowVisible(pWatchedDC->hWnd);
    if (g_bVchanClientConnected) {
        if (bCurrentlyVisible && !pWatchedDC->bVisible)
            send_window_map(pWatchedDC);

        if (!bCurrentlyVisible && pWatchedDC->bVisible)
            send_window_unmap(pWatchedDC->hWnd);
    }

    if (!pWatchedDC->bStyleChecked && (GetTickCount() >= pWatchedDC->uTimeAdded + 500)) {
        pWatchedDC->bStyleChecked = TRUE;

        Style = GetWindowLong(pWatchedDC->hWnd, GWL_STYLE);

        bUpdateStyle = FALSE;
        if (Style & WS_MINIMIZEBOX) {
            Style &= ~WS_MINIMIZEBOX;
            bUpdateStyle = TRUE;
            DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
        }
/*		if (Style & WS_MAXIMIZEBOX) {
            Style &= ~WS_MAXIMIZEBOX;
            bUpdateStyle = TRUE;
            DeleteMenu(GetSystemMenu(pWatchedDC->hWnd, FALSE), SC_MAXIMIZE, MF_BYCOMMAND);
        }
*/
        if (Style & WS_SIZEBOX) {
            Style &= ~WS_SIZEBOX;
            bUpdateStyle = TRUE;
        }

        if (bUpdateStyle) {
            SetWindowLong(pWatchedDC->hWnd, GWL_STYLE, Style);
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

    debugf("success");
    return ERROR_SUCCESS;
}

BOOL ShouldAcceptWindow(HWND hWnd)
{
    LONG Style, ExStyle;

    debugf("0x%x", hWnd);
    if (!IsWindowVisible(hWnd))
        return FALSE;

    Style = GetWindowLong(hWnd, GWL_STYLE);
    ExStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    // If a window has a parent window, has no caption and is not a tool window,
    // then don't show it as a separate window.
    if (GetParent(hWnd) && ((WS_CAPTION & Style) != WS_CAPTION) && !(WS_EX_TOOLWINDOW & ExStyle))
        return FALSE;

    return TRUE;
}

// enums top-level windows and adds them to watch list
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

    if (!ShouldAcceptWindow(hWnd))
        return TRUE;

    if (pBannedPopupsList)
        for (i = 0; i < pBannedPopupsList->uNumberOfBannedPopups; i++)
            if (pBannedPopupsList->hBannedPopupArray[i] == hWnd)
                return TRUE;

    AddWindowWithRect(hWnd, &wi.rcWindow);

    return TRUE;
}

// main function that scans for updates
ULONG ProcessUpdatedWindows(
    BOOLEAN bUpdateEverything
)
{
    PWATCHED_DC pWatchedDC;
    PWATCHED_DC pNextWatchedDC;
    CHAR BannedPopupsListBuffer[sizeof(BANNED_POPUP_WINDOWS) * 4];
    PBANNED_POPUP_WINDOWS pBannedPopupsList = (PBANNED_POPUP_WINDOWS)&BannedPopupsListBuffer;

    debugf("update all? %d", bUpdateEverything);
    if (!g_DesktopHwnd)
        g_DesktopHwnd = GetDesktopWindow();

    if (!g_ExplorerHwnd)
        g_ExplorerHwnd = FindWindow(NULL, _T("Program Manager"));

    if (!g_TaskbarHwnd) {
        g_TaskbarHwnd = FindWindow(_T("Shell_TrayWnd"), NULL);

        if (g_TaskbarHwnd)
            ShowWindow(g_TaskbarHwnd, SW_HIDE);
    }

    if (!g_StartButtonHwnd) {
        g_StartButtonHwnd = FindWindowEx(GetDesktopWindow(), NULL, _T("Button"), NULL);

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
    while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList) {
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

    debugf("success");
    return ERROR_SUCCESS;
}

// g_csWatchedWindows critical section must be entered
PWATCHED_DC AddWindowWithRect(
    HWND hWnd,
    RECT *pRect
)
{
    PWATCHED_DC pWatchedDC = NULL;
    LONG Style;

    debugf("0x%x", hWnd);
    if (!pRect)
        return NULL;

    pWatchedDC = FindWindowByHwnd(hWnd);
    if (pWatchedDC)
        // already being watched
        return pWatchedDC;

    SanitizeRect(pRect, g_ScreenHeight, g_ScreenWidth);

    if ((pRect->top - pRect->bottom == 0) || (pRect->right - pRect->left == 0))
        return NULL;

    pWatchedDC = malloc(sizeof(WATCHED_DC));
    if (!pWatchedDC)
        return NULL;

    ZeroMemory(pWatchedDC, sizeof(WATCHED_DC));

    pWatchedDC->bVisible = IsWindowVisible(hWnd);

    pWatchedDC->bStyleChecked = FALSE;
    pWatchedDC->uTimeAdded = GetTickCount();

    Style = GetWindowLong(hWnd, GWL_STYLE);
    // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (pRect->right-pRect->left == g_ScreenWidth && pRect->bottom-pRect->top == g_ScreenHeight)
        pWatchedDC->bOverrideRedirect = FALSE;
    else
        pWatchedDC->bOverrideRedirect = (BOOL)((WS_CAPTION & Style) != WS_CAPTION);

    pWatchedDC->hWnd = hWnd;
    pWatchedDC->rcWindow = *pRect;

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

    debugf("success");
    return pWatchedDC;
}

PWATCHED_DC AddWindow(
    HWND hWnd
)
{
    PWATCHED_DC pWatchedDC = NULL;
    WINDOWINFO wi;

    debugf("0x%x", hWnd);
    if (hWnd == 0)
        return NULL;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return NULL;

    if (!ShouldAcceptWindow(hWnd))
        return NULL;

    EnterCriticalSection(&g_csWatchedWindows);

    pWatchedDC = AddWindowWithRect(hWnd, &wi.rcWindow);

    LeaveCriticalSection(&g_csWatchedWindows);

    debugf("success");
    return pWatchedDC;
}

ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC)
{
    if (!pWatchedDC)
        return ERROR_INVALID_PARAMETER;

    debugf("hwnd=0x%x, hdc=0x%c", pWatchedDC->hWnd, pWatchedDC->hDC);
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

    logf("destroyed: %x\n", pWatchedDC->hWnd);

    free(pWatchedDC);

    return ERROR_SUCCESS;
}

ULONG RemoveWindow(
    HWND hWnd
)
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

    debugf("success");
    return ERROR_SUCCESS;
}

ULONG CheckWindowUpdates(
    HWND hWnd
)
{
    WINDOWINFO wi;
    PWATCHED_DC pWatchedDC = NULL;

    debugf("0x%x", hWnd);
    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return perror("GetWindowInfo");

    EnterCriticalSection(&g_csWatchedWindows);

    // AddWindowWithRect() returns an existing pWatchedDC if the window is already on the list.
    pWatchedDC = AddWindowWithRect(hWnd, &wi.rcWindow);
    if (!pWatchedDC) {
        logf("AddWindowWithRect returned NULL");
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }

    CheckWatchedWindowUpdates(pWatchedDC, &wi.rcWindow, FALSE);

    LeaveCriticalSection(&g_csWatchedWindows);

    send_wmname(hWnd);

    debugf("success");
    return ERROR_SUCCESS;
}

LRESULT CALLBACK ShellHookWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (uMsg == g_uShellHookMessage) {
        switch (wParam) {
            case HSHELL_WINDOWCREATED:
                AddWindow((HWND) lParam);
                break;

            case HSHELL_WINDOWDESTROYED:
                RemoveWindow((HWND) lParam);
                break;

            case HSHELL_REDRAW:
            case HSHELL_RUDEAPPACTIVATED:
            case HSHELL_WINDOWACTIVATED:
            case HSHELL_GETMINRECT:
                CheckWindowUpdates((HWND) lParam);
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
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
//      _tprintf(_T("hwnd: %x, msg %d\n"), hwnd, uMsg);
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

ULONG CreateShellHookWindow(
    HWND *pHwnd
)
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

    g_uShellHookMessage = RegisterWindowMessage(_T("SHELLHOOK"));

    *pHwnd = hwnd;

    debugf("success");
    return ERROR_SUCCESS;
}

ULONG HookMessageLoop(
)
{
    MSG Msg;
    HINSTANCE hInstance = GetModuleHandle(NULL);

    debugf("start");
    while (GetMessage(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    UnregisterClass(g_szClassName, hInstance);

    debugf("success");
    return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(
    LPVOID pParam
)
{
    if (ERROR_SUCCESS != CreateShellHookWindow(&g_ShellEventsWnd)) {
        return perror("CreateShellHookWindow");
    }

    return HookMessageLoop();
}

ULONG RunShellEventsThread(
)
{
    debugf("start");
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

    g_hShellEventsThread = CreateThread(NULL, 0, ShellEventsThread, NULL, 0, NULL);
    if (!g_hShellEventsThread) {
        return perror("CreateThread(ShellEventsThread)");
    }

    debugf("success");
    return ERROR_SUCCESS;
}

ULONG StopShellEventsThread(
)
{
    if (!g_hShellEventsThread)
        return ERROR_INVALID_PARAMETER;

    debugf("start");
    PostMessage(g_ShellEventsWnd, WM_DESTROY, 0, 0);
    WaitForSingleObject(g_hShellEventsThread, INFINITE);

    CloseHandle(g_hShellEventsThread);

    g_hShellEventsThread = NULL;

    debugf("success");
    return ERROR_SUCCESS;
}
