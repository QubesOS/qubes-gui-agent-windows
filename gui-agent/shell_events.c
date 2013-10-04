#include "shell_events.h"

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

extern	BOOLEAN	g_bVchanClientConnected;

ULONG OpenScreenSection(
)
{
	ULONG uResult;
	TCHAR	SectionName[100];

	StringCchPrintf(SectionName, _countof(SectionName), _T("Global\\QubesSharedMemory_%x"), g_ScreenHeight * g_ScreenWidth * 4);

	g_hSection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
	if (!g_hSection) {
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T(": OpenFileMapping() failed with error %d\n"), uResult);
		return uResult;
	}

	g_pScreenData = MapViewOfFile(g_hSection, FILE_MAP_READ, 0, 0, 0);
	if (!g_pScreenData) {
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T(": MapViewOfFile() failed with error %d\n"), uResult);
		return uResult;
	}

	return ERROR_SUCCESS;
}


PWATCHED_DC FindWindowByHwnd(
	HWND hWnd
)
{
	PWATCHED_DC pWatchedDC;

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
	RECT * pRect,
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
	PUCHAR pCompositionBuffer,
	RECT * pRect
)
{
	LONG Line;
	ULONG uWindowStride;
	ULONG uScreenStride;
	PUCHAR pSourceLine = NULL;
	PUCHAR pDestLine = NULL;

	if (!pCompositionBuffer || !g_pScreenData)
		return ERROR_INVALID_PARAMETER;

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
	RECT * pRect,
	BOOLEAN bDamageDetected
)
{
	WINDOWINFO wi;
	BOOLEAN	bResizingDetected;

	if (!pWatchedDC)
		return ERROR_INVALID_PARAMETER;

	if (!pRect) {
		wi.cbSize = sizeof(wi);
		if (!GetWindowInfo(pWatchedDC->hWnd, &wi))
			return GetLastError();
	} else
		wi.rcWindow = *pRect;

	SanitizeRect(&wi.rcWindow, pWatchedDC->MaxHeight, pWatchedDC->MaxWidth);

	bDamageDetected |= wi.rcWindow.left != pWatchedDC->rcWindow.left ||
		wi.rcWindow.top != pWatchedDC->rcWindow.top ||
		wi.rcWindow.right != pWatchedDC->rcWindow.right || wi.rcWindow.bottom != pWatchedDC->rcWindow.bottom;


	bResizingDetected = (wi.rcWindow.right - wi.rcWindow.left != pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left) ||
		(wi.rcWindow.bottom - wi.rcWindow.top != pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);


	if (bDamageDetected || bResizingDetected) {

//		_tprintf(_T("hwnd: %x, left: %d top: %d right: %d bottom %d\n"), pWatchedDC->hWnd, wi.rcWindow.left, wi.rcWindow.top, wi.rcWindow.right,
//			 wi.rcWindow.bottom);

		pWatchedDC->rcWindow = wi.rcWindow;
		CopyScreenData(pWatchedDC->pCompositionBuffer, &pWatchedDC->rcWindow);

		if (g_bVchanClientConnected) {
			send_window_configure(pWatchedDC);

			if (bResizingDetected)
				send_pixmap_mfns(pWatchedDC);

			send_window_damage_event(pWatchedDC->hWnd, 0, 0, pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left, pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top);
		}
	}

	return ERROR_SUCCESS;
}

ULONG ProcessUpdatedWindows(
	BOOLEAN bUpdateEverything
)
{
	PWATCHED_DC pWatchedDC;

	EnterCriticalSection(&g_csWatchedWindows);

	pWatchedDC = (PWATCHED_DC) g_WatchedWindowsList.Flink;
	while (pWatchedDC != (PWATCHED_DC) & g_WatchedWindowsList) {
		pWatchedDC = CONTAINING_RECORD(pWatchedDC, WATCHED_DC, le);

		CheckWatchedWindowUpdates(pWatchedDC, NULL, bUpdateEverything);

		pWatchedDC = (PWATCHED_DC) pWatchedDC->le.Flink;
	}

	LeaveCriticalSection(&g_csWatchedWindows);

	return ERROR_SUCCESS;
}

// g_csWatchedWindows critical section must be entered
PWATCHED_DC AddWindowWithRect(
	HWND hWnd,
	RECT * pRect
)
{
	PWATCHED_DC pWatchedDC = NULL;
	ULONG uResult;

	if (!pRect)
		return NULL;

	pWatchedDC = FindWindowByHwnd(hWnd);
	if (pWatchedDC)
		// already here
		return pWatchedDC;

	SanitizeRect(pRect, g_ScreenHeight, g_ScreenWidth);

	if ((pRect->top - pRect->bottom == 0) || (pRect->right - pRect->left == 0))
		return NULL;

	pWatchedDC = malloc(sizeof(WATCHED_DC));
	if (!pWatchedDC)
		return NULL;

	memset(pWatchedDC, 0, sizeof(WATCHED_DC));

	pWatchedDC->hWnd = hWnd;
	pWatchedDC->rcWindow = *pRect;

	pWatchedDC->MaxHeight = g_ScreenHeight;
	pWatchedDC->MaxWidth = g_ScreenWidth;

	pWatchedDC->uCompositionBufferSize = pWatchedDC->MaxHeight * pWatchedDC->MaxWidth * 4;

	pWatchedDC->pCompositionBuffer = VirtualAlloc(NULL, pWatchedDC->uCompositionBufferSize, MEM_COMMIT, PAGE_READWRITE);
	if (!pWatchedDC->pCompositionBuffer) {
		_tprintf(_T(__FUNCTION__) _T("(): VirtualAlloc(%d) failed, error %d\n"), pWatchedDC->uCompositionBufferSize, GetLastError());
		free(pWatchedDC);
		return NULL;
	}

	if (!VirtualLock(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize)) {
		_tprintf(_T(__FUNCTION__) _T("(): VirtualLock() failed, error %d\n"), GetLastError());
		VirtualFree(pWatchedDC->pCompositionBuffer, 0, MEM_RELEASE);
		free(pWatchedDC);
		return NULL;
	}

	uResult = GetPfnList(pWatchedDC->pCompositionBuffer, pWatchedDC->uCompositionBufferSize, &pWatchedDC->PfnArray);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("(): GetPfnList() failed, error %d\n"), uResult);
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

	_tprintf(_T("created: %x, pages: %d: %d %d %d\n"), hWnd, pWatchedDC->PfnArray.uNumberOf4kPages, pWatchedDC->PfnArray.Pfn[0], pWatchedDC->PfnArray.Pfn[1], pWatchedDC->PfnArray.Pfn[2]);


	CopyScreenData(pWatchedDC->pCompositionBuffer, &pWatchedDC->rcWindow);

	if (g_bVchanClientConnected) {
		send_window_create(pWatchedDC);
		send_pixmap_mfns(pWatchedDC);
	}

	InsertTailList(&g_WatchedWindowsList, &pWatchedDC->le);

//	_tprintf(_T("created: %x, left: %d top: %d right: %d bottom %d\n"), hWnd, pWatchedDC->rcWindow.left, pWatchedDC->rcWindow.top,
//		 pWatchedDC->rcWindow.right, pWatchedDC->rcWindow.bottom);
	return pWatchedDC;
}

PWATCHED_DC AddWindow(
	HWND hWnd
)
{
	PWATCHED_DC pWatchedDC = NULL;
	WINDOWINFO wi;
	ULONG uResult;

	if (hWnd == 0)
		return NULL;

	wi.cbSize = sizeof(wi);
	if (!GetWindowInfo(hWnd, &wi))
		return NULL;

	EnterCriticalSection(&g_csWatchedWindows);

	pWatchedDC = AddWindowWithRect(hWnd, &wi.rcWindow);

	LeaveCriticalSection(&g_csWatchedWindows);

	return pWatchedDC;
}

ULONG RemoveWindow(
	HWND hWnd
)
{
	PWATCHED_DC pWatchedDC;
	ULONG uResult;

	EnterCriticalSection(&g_csWatchedWindows);

	pWatchedDC = FindWindowByHwnd(hWnd);
	if (!pWatchedDC) {
		LeaveCriticalSection(&g_csWatchedWindows);
		return ERROR_SUCCESS;
	}

	RemoveEntryList(&pWatchedDC->le);

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

	_tprintf(_T("destroyed: %x\n"), hWnd);

	LeaveCriticalSection(&g_csWatchedWindows);

	return ERROR_SUCCESS;
}

ULONG CheckWindowUpdates(
	HWND hWnd
)
{
	WINDOWINFO wi;
	PWATCHED_DC pWatchedDC = NULL;

	wi.cbSize = sizeof(wi);
	if (!GetWindowInfo(hWnd, &wi))
		return GetLastError();

	EnterCriticalSection(&g_csWatchedWindows);

	// AddWindowWithRect() returns an existing pWatchedDC if the window is already on the list.
	pWatchedDC = AddWindowWithRect(hWnd, &wi.rcWindow);
	if (!pWatchedDC) {
		LeaveCriticalSection(&g_csWatchedWindows);
		return ERROR_SUCCESS;
	}

	CheckWatchedWindowUpdates(pWatchedDC, &wi.rcWindow, FALSE);

	LeaveCriticalSection(&g_csWatchedWindows);

	return ERROR_SUCCESS;
}

LRESULT CALLBACK WndProc(
	HWND hwnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam
)
{
	ULONG uThreadId;
	WINDOWINFO wiScreen;

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

		case HSHELL_WINDOWREPLACING:
		case HSHELL_WINDOWREPLACED:
		case HSHELL_FLASH:
		case HSHELL_ENDTASK:
		case HSHELL_APPCOMMAND:
			break;
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
//              _tprintf(_T("hwnd: %x, msg %d\n"), hwnd, uMsg);
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
	return 0;
}

ULONG CreateShellHookWindow(
	HWND * pHwnd
)
{
	WNDCLASSEX wc;
	HWND hwnd;
	ULONG uResult;
	HINSTANCE hInstance = GetModuleHandle(NULL);

	if (!pHwnd)
		return ERROR_INVALID_PARAMETER;

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = g_szClassName;

	if (!RegisterClassEx(&wc)) {
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T("(): Could not register a window class, error %d\n"), uResult);
		return uResult;
	}

	hwnd = CreateWindow(g_szClassName, _T("QubesShellHook"), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInstance, NULL);

	if (hwnd == NULL) {
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T("(): Could not create a window, error %d\n"), uResult);
		return uResult;
	}

	ShowWindow(hwnd, SW_HIDE);
	UpdateWindow(hwnd);

	if (!RegisterShellHookWindow(hwnd)) {
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T("(): RegisterShellHookWindow() failed, error %d\n"), uResult);
		DestroyWindow(hwnd);
		UnregisterClass(g_szClassName, hInstance);
		return uResult;
	}

	g_uShellHookMessage = RegisterWindowMessage(_T("SHELLHOOK"));

	*pHwnd = hwnd;

	return ERROR_SUCCESS;
}

ULONG WatchForShellEvents(
)
{
	MSG Msg;
	HINSTANCE hInstance = GetModuleHandle(NULL);

	while (GetMessage(&Msg, NULL, 0, 0) > 0) {
		TranslateMessage(&Msg);
		DispatchMessage(&Msg);
	}

	UnregisterClass(g_szClassName, hInstance);

	return ERROR_SUCCESS;
}

ULONG WINAPI ShellEventsThread(
	LPVOID pParam
)
{
	ULONG uResult;

	uResult = CreateShellHookWindow(&g_ShellEventsWnd);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("(): CreateShellHookWindow() failed, error %d\n"), uResult);
		return uResult;
	}

	return WatchForShellEvents();
}

ULONG RunShellEventsThread(
)
{
	ULONG uResult;

	uResult = OpenScreenSection();
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("(): Could not open a shared desktop section, error %d\n"), uResult);
		return uResult;
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
		uResult = GetLastError();
		_tprintf(_T(__FUNCTION__) _T("(): Could not create a shell hook thread, error %d\n"), uResult);
		return uResult;
	}

	return ERROR_SUCCESS;
}

ULONG StopShellEventsThread(
)
{
	if (!g_hShellEventsThread)
		return ERROR_INVALID_PARAMETER;

	PostMessage(g_ShellEventsWnd, WM_DESTROY, 0, 0);
	WaitForSingleObject(g_hShellEventsThread, INFINITE);

	CloseHandle(g_hShellEventsThread);

	g_hShellEventsThread = NULL;
	return ERROR_SUCCESS;
}
