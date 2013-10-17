#define OEMRESOURCE 
#include <windows.h>
#include "tchar.h"
#include "qubes-gui-protocol.h"
#include "libvchan.h"
#include "glue.h"
#include "log.h"
#include "qvcontrol.h"
#include "shell_events.h"
#include "resource.h"

#define lprintf_err	Lprintf_err
#define lprintf	Lprintf

HANDLE g_hStopServiceEvent;
HANDLE g_hCleanupFinishedEvent;

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

extern LONG g_ScreenWidth;
extern LONG g_ScreenHeight;

extern HANDLE g_hSection;
extern PUCHAR g_pScreenData;

BOOLEAN	g_bVchanClientConnected = FALSE;

/* Get PFNs of hWnd Window from QVideo driver and prepare relevant shm_cmd
 * struct.
 */
ULONG PrepareShmCmd(
	PWATCHED_DC pWatchedDC,
	struct shm_cmd ** ppShmCmd
)
{
	QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
	ULONG uResult;
	ULONG uShmCmdSize = 0;
	struct shm_cmd *pShmCmd = NULL;
	PPFN_ARRAY	pPfnArray = NULL;
	HWND	hWnd = 0;
	ULONG	uWidth;
	ULONG	uHeight;
	ULONG	ulBitCount;
	BOOLEAN	bIsScreen;
	ULONG	i;

	if (!ppShmCmd)
		return ERROR_INVALID_PARAMETER;
	*ppShmCmd = NULL;

	if (!pWatchedDC) {

		memset(&QvGetSurfaceDataResponse, 0, sizeof(QvGetSurfaceDataResponse));

		uResult = GetWindowData(hWnd, &QvGetSurfaceDataResponse);
		if (ERROR_SUCCESS != uResult) {
			_tprintf(_T(__FUNCTION__) _T("GetWindowData() failed with error %d\n"), uResult);
			return uResult;
		}

		uWidth = QvGetSurfaceDataResponse.cx;
		uHeight = QvGetSurfaceDataResponse.cy;
		ulBitCount = QvGetSurfaceDataResponse.ulBitCount;

		bIsScreen = TRUE;

		pPfnArray = &QvGetSurfaceDataResponse.PfnArray;

	} else {

		hWnd = pWatchedDC->hWnd;

		uWidth = pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left;
		uHeight = pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top;
		ulBitCount = 32;

		bIsScreen = FALSE;

		pPfnArray = &pWatchedDC->PfnArray;
	}


	_tprintf(_T(__FUNCTION__) _T("Window %dx%d, %d bpp, screen: %d\n"), uWidth, uHeight,
		 ulBitCount, bIsScreen);
	_tprintf(_T(__FUNCTION__) _T("PFNs: %d; 0x%x, 0x%x, 0x%x\n"), pPfnArray->uNumberOf4kPages,
		 pPfnArray->Pfn[0], pPfnArray->Pfn[1], pPfnArray->Pfn[2]);

	uShmCmdSize = sizeof(struct shm_cmd) + pPfnArray->uNumberOf4kPages * sizeof(uint32_t);

	pShmCmd = malloc(uShmCmdSize);
	if (!pShmCmd) {
		_tprintf(_T(__FUNCTION__) _T("Failed to allocate %d bytes for shm_cmd for window 0x%x\n"), uShmCmdSize, hWnd);
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	memset(pShmCmd, 0, uShmCmdSize);


	pShmCmd->shmid = 0;
	pShmCmd->width = uWidth;
	pShmCmd->height = uHeight;
	pShmCmd->bpp = ulBitCount;
	pShmCmd->off = 0;
	pShmCmd->num_mfn = pPfnArray->uNumberOf4kPages;
	pShmCmd->domid = 0;

	for (i = 0; i < pPfnArray->uNumberOf4kPages; i++)
		pShmCmd->mfns[i] = (uint32_t)pPfnArray->Pfn[i];

	*ppShmCmd = pShmCmd;

	return ERROR_SUCCESS;
}

void send_pixmap_mfns(
	PWATCHED_DC pWatchedDC
)
{
	ULONG uResult;
	struct shm_cmd *pShmCmd = NULL;
	struct msg_hdr hdr;
	int size;
	HWND	hWnd = 0;

	if (pWatchedDC)
		hWnd = pWatchedDC->hWnd;

	uResult = PrepareShmCmd(pWatchedDC, &pShmCmd);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("PrepareShmCmd() failed with error %d\n"), uResult);
		return;
	}

	if (pShmCmd->num_mfn == 0 || pShmCmd->num_mfn > MAX_MFN_COUNT) {
		_tprintf(_T(__FUNCTION__) _T("got num_mfn=0x%x for window 0x%x\n"), pShmCmd->num_mfn, (int)hWnd);
		free(pShmCmd);
		return;
	}

	size = pShmCmd->num_mfn * sizeof(uint32_t);

	hdr.type = MSG_MFNDUMP;
	hdr.window = (uint32_t) hWnd;
	hdr.untrusted_len = sizeof(struct shm_cmd) + size;
	write_struct(hdr);
	write_data(pShmCmd, sizeof(struct shm_cmd) + size);

	free(pShmCmd);
}

ULONG send_window_create(
	PWATCHED_DC	pWatchedDC
)
{
	WINDOWINFO wi;
	ULONG uResult;
	struct msg_hdr hdr;
	struct msg_create mc;
	struct msg_map_info mmi;

	wi.cbSize = sizeof(wi);
	/* special case for full screen */
	if (pWatchedDC == NULL) {
		QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
		ULONG uResult;

		/* TODO: multiple screens? */
		wi.rcWindow.left = 0;
		wi.rcWindow.top = 0;

		memset(&QvGetSurfaceDataResponse, 0, sizeof(QvGetSurfaceDataResponse));

		uResult = GetWindowData(NULL, &QvGetSurfaceDataResponse);
		if (ERROR_SUCCESS != uResult) {
			_tprintf(_T(__FUNCTION__) _T("GetWindowData() failed with error %d\n"), uResult);
			return uResult;
		}

		wi.rcWindow.right = QvGetSurfaceDataResponse.cx;
		wi.rcWindow.bottom = QvGetSurfaceDataResponse.cy;

		hdr.window = 0;

	} else {
		hdr.window = (uint32_t)pWatchedDC->hWnd;
		wi.rcWindow = pWatchedDC->rcWindow;
	}

	hdr.type = MSG_CREATE;

	mc.x = wi.rcWindow.left;
	mc.y = wi.rcWindow.top;
	mc.width = wi.rcWindow.right - wi.rcWindow.left;
	mc.height = wi.rcWindow.bottom - wi.rcWindow.top;
	mc.parent = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
	mc.override_redirect = 0;

	write_message(hdr, mc);

	/* FIXME: for now, all windows are imediately mapped, but this should be
	 * changed to reflect real window visibility */
	mmi.transient_for = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
	mmi.override_redirect = 0;

	hdr.type = MSG_MAP;
	write_message(hdr, mmi);

	return ERROR_SUCCESS;
}

ULONG send_window_destroy(
	HWND window
)
{
	struct msg_hdr hdr;


	hdr.type = MSG_DESTROY;
	hdr.window = (uint32_t)window;
	hdr.untrusted_len = 0;
	write_struct(hdr);

	return ERROR_SUCCESS;
}

ULONG send_window_unmap(
	HWND window
)
{
	struct msg_hdr hdr;


	hdr.type = MSG_UNMAP;
	hdr.window = (uint32_t)window;
	hdr.untrusted_len = 0;
	write_struct(hdr);

	return ERROR_SUCCESS;
}

ULONG send_window_configure(
	PWATCHED_DC	pWatchedDC
)
{
	ULONG uResult;
	struct msg_hdr hdr;
	struct msg_configure mc;
	struct msg_map_info mmi;


	hdr.window = (uint32_t)pWatchedDC->hWnd;

	hdr.type = MSG_CONFIGURE;

	mc.x = pWatchedDC->rcWindow.left;
	mc.y = pWatchedDC->rcWindow.top;
	mc.width = pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left;
	mc.height = pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top;
	mc.override_redirect = 0;

	write_message(hdr, mc);

	/* FIXME: for now, all windows are imediately mapped, but this should be
	 * changed to reflect real window visibility */
	mmi.transient_for = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
	mmi.override_redirect = 0;

	hdr.type = MSG_MAP;
	write_message(hdr, mmi);

	return ERROR_SUCCESS;
}


void send_window_damage_event(
	HWND window,
	int x,
	int y,
	int width,
	int height
)
{
	struct msg_shmimage mx;
	struct msg_hdr hdr;

	hdr.type = MSG_SHMIMAGE;
	hdr.window = (uint32_t) window;
	mx.x = x;
	mx.y = y;
	mx.width = width;
	mx.height = height;
	write_message(hdr, mx);
}

void send_protocol_version(
)
{
	uint32_t version = QUBES_GUI_PROTOCOL_VERSION_WINDOWS;
	write_struct(version);
}

ULONG SetVideoMode(uWidth, uHeight, uBpp)
{
	ULONG uResult;
	LPTSTR ptszDeviceName = NULL;
	DISPLAY_DEVICE DisplayDevice;

	if (!IS_RESOLUTION_VALID(uWidth, uHeight)) {
		_tprintf(_T("Resolution is invalid: %dx%d\n"), uWidth, uHeight);
		return ERROR_INVALID_PARAMETER;
	}

	_tprintf(_T("New resolution: %dx%d bpp %d\n"), uWidth, uHeight, uBpp);

	uResult = FindQubesDisplayDevice(&DisplayDevice);
	if (ERROR_SUCCESS != uResult) {
		Lprintf_err(uResult, "FindQubesDisplayDevice() failed with error\n");
		return uResult;
	}
	ptszDeviceName = (LPTSTR) & DisplayDevice.DeviceName[0];

	Lprintf("DeviceName: %S\n\n", ptszDeviceName);

	uResult = SupportVideoMode(ptszDeviceName, uWidth, uHeight, uBpp);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T("SupportVideoMode() failed with error %d\n"), uResult);
		return uResult;
	}

	uResult = ChangeVideoMode(ptszDeviceName, uWidth, uHeight, uBpp);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T("ChangeVideoMode() failed with error %d\n"), uResult);
		return uResult;
	}

	_tprintf(_T("Video mode changed successfully.\n"));
	return ERROR_SUCCESS;
}

void handle_xconf(
)
{
	struct msg_xconf xconf;
	ULONG	uResult;

	read_all_vchan_ext((char *)&xconf, sizeof(xconf));

	printf("host resolution: %dx%d, mem: %d, depth: %d\n", xconf.w, xconf.h, xconf.mem, xconf.depth);

	uResult = SetVideoMode(xconf.w, xconf.h, 32);
	if (ERROR_SUCCESS != uResult) {
		QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;

		_tprintf(_T("handle_xconf: SetVideoMode(): %d\n"), uResult);

		memset(&QvGetSurfaceDataResponse, 0, sizeof(QvGetSurfaceDataResponse));

		uResult = GetWindowData(0, &QvGetSurfaceDataResponse);
		if (ERROR_SUCCESS != uResult) {
			_tprintf(_T(__FUNCTION__) _T("GetWindowData() failed with error %d\n"), uResult);
			return;
		}

		g_ScreenWidth = QvGetSurfaceDataResponse.cx;
		g_ScreenHeight = QvGetSurfaceDataResponse.cy;

		_tprintf(_T("handle_xconf: keeping original %dx%d\n"), g_ScreenWidth, g_ScreenHeight);
	} else {

		g_ScreenWidth = xconf.w;
		g_ScreenHeight = xconf.h;

	}
	RunShellEventsThread();
}


void handle_keypress(HWND window)
{
    struct msg_keypress key;
	INPUT inputEvent;
    read_all_vchan_ext((char *) &key, sizeof(key));

	/* ignore x, y */
	/* TODO: send to correct window */
	/* TODO: handle key.state */

	inputEvent.type = INPUT_KEYBOARD;
	inputEvent.ki.time = 0;
	inputEvent.ki.wScan = 0; /* TODO? */
	inputEvent.ki.wVk = X11ToVk[key.keycode];
	inputEvent.ki.dwFlags = key.type == KeyPress ? 0 : KEYEVENTF_KEYUP;
	inputEvent.ki.dwExtraInfo = 0;

	if (!SendInput(1, &inputEvent, sizeof(inputEvent))) {
		lprintf_err(GetLastError(), "handle_keypress: SendInput()");
		return;
	}
}

void handle_button(HWND window)
{
	struct msg_button button;
	INPUT inputEvent;
	RECT	rect = {0};

	read_all_vchan_ext((char *) &button, sizeof(button));

	if (window)
		GetWindowRect(window, &rect);

	/* TODO: send to correct window */

	inputEvent.type = INPUT_MOUSE;
	inputEvent.mi.time = 0;
	inputEvent.mi.mouseData = 0;
	inputEvent.mi.dwExtraInfo = 0;
	/* pointer coordinates must be 0..65535, which covers the whole screen -
	 * regardless of resolution */
	inputEvent.mi.dx = (button.x + rect.left) * 65535 / g_ScreenWidth;
	inputEvent.mi.dy = (button.y + rect.top) * 65535 / g_ScreenHeight;
	switch (button.button) {
		case Button1:
			inputEvent.mi.dwFlags =
				(button.type == ButtonPress) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
			break;
		case Button2:
			inputEvent.mi.dwFlags =
				(button.type == ButtonPress) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
			break;
		case Button3:
			inputEvent.mi.dwFlags =
				(button.type == ButtonPress) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
			break;
		case Button4:
		case Button5:
			inputEvent.mi.dwFlags = MOUSEEVENTF_WHEEL;
			inputEvent.mi.mouseData = (button.button == Button4) ? WHEEL_DELTA : -WHEEL_DELTA;
			break;
		default:
			_tprintf(_T(__FUNCTION__) _T("unknown button pressed/released 0x%x\n"), button.button);
	}
	if (!SendInput(1, &inputEvent, sizeof(inputEvent))) {
		lprintf_err(GetLastError(), "handle_keypress: SendInput()");
		return;
	}
}

void handle_motion(HWND window)
{
	struct msg_motion motion;
	INPUT inputEvent;
	RECT	rect = {0};

	read_all_vchan_ext((char *) &motion, sizeof(motion));

	if (window)
		GetWindowRect(window, &rect);

	inputEvent.type = INPUT_MOUSE;
	inputEvent.mi.time = 0;
	/* pointer coordinates must be 0..65535, which covers the whole screen -
	 * regardless of resolution */
	inputEvent.mi.dx = (motion.x + rect.left) * 65535 / g_ScreenWidth;
	inputEvent.mi.dy = (motion.y + rect.top) * 65535 / g_ScreenHeight;
	inputEvent.mi.mouseData = 0;
	inputEvent.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_ABSOLUTE;
	inputEvent.mi.dwExtraInfo = 0;

	if (!SendInput(1, &inputEvent, sizeof(inputEvent))) {
		lprintf_err(GetLastError(), "handle_keypress: SendInput()");
		return;
	}
}

void handle_configure(HWND window)
{
	struct msg_configure configure;

	read_all_vchan_ext((char *) &configure, sizeof(configure));
	SetWindowPos(window, HWND_TOP, configure.x, configure.y, configure.width, configure.height, 0);
}

void handle_focus(HWND window)
{
	struct msg_focus focus;

	read_all_vchan_ext((char *) &focus, sizeof(focus));

	BringWindowToTop(window);
	SetForegroundWindow(window);
        SetActiveWindow(window);
	SetFocus(window);

}

void handle_close(HWND window)
{
	PostMessage(window, WM_SYSCOMMAND, SC_CLOSE, 0);
}

ULONG handle_server_data(
)
{
	struct msg_hdr hdr;
	char discard[256];
	int nbRead;
	read_all_vchan_ext((char *)&hdr, sizeof(hdr));

#ifdef DBG
	_tprintf(_T(__FUNCTION__) _T("received message type %d for 0x%x\n"), hdr.type, hdr.window);
#endif

	switch (hdr.type) {
	case MSG_KEYPRESS:
		handle_keypress((HWND)hdr.window);
		break;
	case MSG_BUTTON:
		handle_button((HWND)hdr.window);
		break;
	case MSG_MOTION:
		handle_motion((HWND)hdr.window);
		break;
	case MSG_CONFIGURE:
		handle_configure((HWND)hdr.window);
		break;
	case MSG_FOCUS:
		handle_focus((HWND)hdr.window);
		break;
	case MSG_CLOSE:
		handle_close((HWND)hdr.window);
		break;
	default:
		_tprintf(_T(__FUNCTION__) _T("got unknown msg type %d, ignoring\n"), hdr.type);

	case MSG_MAP:
//              handle_map(g, hdr.window);
//		break;
	case MSG_CROSSING:
//              handle_crossing(g, hdr.window);
//		break;
	case MSG_CLIPBOARD_REQ:
//              handle_clipboard_req(g, hdr.window);
//		break;
	case MSG_CLIPBOARD_DATA:
//              handle_clipboard_data(g, hdr.window);
//		break;
	case MSG_EXECUTE:
//              handle_execute();
//		break;
	case MSG_KEYMAP_NOTIFY:
//              handle_keymap_notify(g);
//		break;
	case MSG_WINDOW_FLAGS:
//              handle_window_flags(g, hdr.window);
//		break;
		/* discard unsupported message body */
		while (hdr.untrusted_len > 0) {
			nbRead = read_all_vchan_ext(discard, min(hdr.untrusted_len, sizeof(discard)));
			if (nbRead <= 0)
				break;
			hdr.untrusted_len -= nbRead;
		}
	}

	return ERROR_SUCCESS;
}

ULONG WatchForEvents(
)
{
	EVTCHN evtchn;
	OVERLAPPED ol;
	unsigned int fired_port;
	ULONG uEventNumber;
	DWORD i, dwSignaledEvent;
	BOOLEAN bVchanIoInProgress;
	ULONG uResult;
	BOOLEAN bVchanReturnedError;
//	BOOLEAN bVchanClientConnected;
	HANDLE WatchedEvents[MAXIMUM_WAIT_OBJECTS];
	HANDLE hWindowDamageEvent;
	HDC hDC;
	ULONG uDamage = 0;

	HWND	hWnd;
	struct shm_cmd *pShmCmd = NULL;



	hWindowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	// This will not block.
	uResult = peer_server_init(6000);
	if (uResult) {
		lprintf_err(ERROR_INVALID_FUNCTION, "WatchForEvents(): peer_server_init()");
		return ERROR_INVALID_FUNCTION;
	}

	lprintf("WatchForEvents(): Awaiting for a vchan client, write ring size: %d\n", buffer_space_vchan_ext());

	evtchn = libvchan_fd_for_select(ctrl);

	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	g_bVchanClientConnected = FALSE;
	bVchanIoInProgress = FALSE;
	bVchanReturnedError = FALSE;

	for (;;) {

		uEventNumber = 0;

		// Order matters.
		WatchedEvents[uEventNumber++] = g_hStopServiceEvent;
		WatchedEvents[uEventNumber++] = hWindowDamageEvent;

		uResult = ERROR_SUCCESS;

		libvchan_prepare_to_select(ctrl);
		// read 1 byte instead of sizeof(fired_port) to not flush fired port
		// from evtchn buffer; evtchn driver will read only whole fired port
		// numbers (sizeof(fired_port)), so this will end in zero-length read
		if (!ReadFile(evtchn, &fired_port, 1, NULL, &ol)) {
			uResult = GetLastError();
			if (ERROR_IO_PENDING != uResult) {
				lprintf_err(uResult, "WatchForEvents(): Vchan async read");
				bVchanReturnedError = TRUE;
				break;
			}
		}

		bVchanIoInProgress = TRUE;

		if (ERROR_SUCCESS == uResult || ERROR_IO_PENDING == uResult)
			WatchedEvents[uEventNumber++] = ol.hEvent;

		dwSignaledEvent = WaitForMultipleObjects(uEventNumber, WatchedEvents, FALSE, INFINITE);
		if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS) {

			uResult = GetLastError();
			lprintf_err(uResult, "WatchForEvents(): WaitForMultipleObjects()");
			break;

		} else {

			if (0 == dwSignaledEvent)
				// g_hStopServiceEvent is signaled
				break;

//                      lprintf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
			switch (dwSignaledEvent) {
			case 1:

#ifdef DBG
				logprintf("Damage %d\n", uDamage++);
#endif
				if (g_bVchanClientConnected) {
//					send_window_damage_event(NULL, 0, 0, g_ScreenWidth, g_ScreenHeight);
					ProcessUpdatedWindows(TRUE);
				}
				break;

			case 2:
				// the following will never block; we need to do this to
				// clear libvchan_fd pending state
				//
				// using libvchan_wait here instead of reading fired
				// port at the beginning of the loop (ReadFile call) to be
				// sure that we clear pending state _only_
				// when handling vchan data in this loop iteration (not any
				// other process)
				libvchan_wait(ctrl);

				bVchanIoInProgress = FALSE;

				if (!g_bVchanClientConnected) {

					lprintf("WatchForEvents(): A vchan client has connected\n");

					// Remove the xenstore device/vchan/N entry.
					uResult = libvchan_server_handle_connected(ctrl);
					if (uResult) {
						lprintf_err(ERROR_INVALID_FUNCTION, "WatchForEvents(): libvchan_server_handle_connected()");
						bVchanReturnedError = TRUE;
						break;
					}

					send_protocol_version();

					// This will probably change the current video mode.
					handle_xconf();

					// The desktop DC should be opened only after the resolution changes.
					hDC = GetDC(NULL);
					uResult = RegisterWatchedDC(hDC, hWindowDamageEvent);
					if (ERROR_SUCCESS != uResult)
						lprintf_err(uResult, "WatchForEvents(): RegisterWatchedDC()");

//					send_window_create(NULL);
//					send_pixmap_mfns(NULL);

					g_bVchanClientConnected = TRUE;
					break;
				}

				if (!GetOverlappedResult(evtchn, &ol, &i, FALSE)) {
					if (GetLastError() == ERROR_IO_DEVICE) {
						// in case of ring overflow, above libvchan_wait
						// already reseted the evtchn ring, so ignore this
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
					} else if (GetLastError() != ERROR_OPERATION_ABORTED) {
						lprintf_err(GetLastError(), "WatchForEvents(): GetOverlappedResult(evtchn)");
						bVchanReturnedError = TRUE;
						break;
					}
				}

				if (libvchan_is_eof(ctrl)) {
					bVchanReturnedError = TRUE;
					break;
				}

				while (read_ready_vchan_ext()) {
					uResult = handle_server_data();
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "WatchForEvents(): handle_server_data()");
						break;
					}
				}

				break;

			}
		}

		if (bVchanReturnedError)
			break;

	}

	if (bVchanIoInProgress)
		if (CancelIo(evtchn))
			// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
			// OVERLAPPED structure.
			WaitForSingleObject(ol.hEvent, INFINITE);

	if (!g_bVchanClientConnected)
		// Remove the xenstore device/vchan/N entry.
		libvchan_server_handle_connected(ctrl);

	if (g_bVchanClientConnected)
		libvchan_close(ctrl);

	// This is actually CloseHandle(evtchn)

	xc_evtchn_close(ctrl->evfd);

	CloseHandle(ol.hEvent);
	CloseHandle(hWindowDamageEvent);

	UnregisterWatchedDC(hDC);
	ReleaseDC(NULL, hDC);

	return bVchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static ULONG CheckForXenInterface(
)
{
	EVTCHN xc;

	xc = xc_evtchn_open();
	if (INVALID_HANDLE_VALUE == xc)
		return ERROR_NOT_SUPPORTED;
	xc_evtchn_close(xc);
	return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(
	DWORD fdwCtrlType
)
{
	Lprintf("CtrlHandler(): Got shutdown signal\n");

	SetEvent(g_hStopServiceEvent);

	WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

	CloseHandle(g_hStopServiceEvent);
	CloseHandle(g_hCleanupFinishedEvent);

	StopShellEventsThread();

	Lprintf("CtrlHandler(): Shutdown complete\n");
	ExitProcess(0);
	return TRUE;
}


ULONG IncreaseProcessWorkingSetSize(SIZE_T uNewMinimumWorkingSetSize, SIZE_T uNewMaximumWorkingSetSize)
{
	SIZE_T	uMinimumWorkingSetSize = 0;
	SIZE_T	uMaximumWorkingSetSize = 0;
	ULONG	uResult;


	if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize)) {
		uResult = GetLastError();
		lprintf_err(uResult, "IncreaseProcessWorkingSetSize(): GetProcessWorkingSetSize() failed, error %d\n", uResult);
		return uResult;
	}

	if (!SetProcessWorkingSetSize(GetCurrentProcess(), uNewMinimumWorkingSetSize, uNewMaximumWorkingSetSize)) {
		uResult = GetLastError();
		lprintf_err(uResult, "IncreaseProcessWorkingSetSize(): SetProcessWorkingSetSize() failed, error %d\n", uResult);
		return uResult;
	}

	if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize)) {
		uResult = GetLastError();
		lprintf_err(uResult, "IncreaseProcessWorkingSetSize(): GetProcessWorkingSetSize() failed, error %d\n", uResult);
		return uResult;
	}

	logprintf("IncreaseProcessWorkingSetSize(): New working set size: %d pages\n", uMaximumWorkingSetSize >> 12);

	return ERROR_SUCCESS;
}


ULONG HideCursors()
{
	HCURSOR	hBlankCursor;
	HCURSOR	hBlankCursorCopy;
	ULONG	uResult;
	UCHAR	i;
	ULONG	CursorsToHide[] = {
		OCR_APPSTARTING,	// Standard arrow and small hourglass
		OCR_NORMAL,		// Standard arrow
		OCR_CROSS,		// Crosshair
		OCR_HAND,		// Hand
		OCR_IBEAM,		// I-beam
		OCR_NO,			// Slashed circle
		OCR_SIZEALL,		// Four-pointed arrow pointing north, south, east, and west
		OCR_SIZENESW,		// Double-pointed arrow pointing northeast and southwest
		OCR_SIZENS,		// Double-pointed arrow pointing north and south
		OCR_SIZENWSE,		// Double-pointed arrow pointing northwest and southeast
		OCR_SIZEWE,		// Double-pointed arrow pointing west and east
		OCR_UP,			// Vertical arrow
		OCR_WAIT		// Hourglass
	};

	hBlankCursor = LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_BLANK), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE);
	if (!hBlankCursor) {
		uResult = GetLastError();
		lprintf_err(uResult, "HideCursors(): LoadImage() failed, error %d\n", uResult);
		return uResult;
	}

	for (i = 0; i < RTL_NUMBER_OF(CursorsToHide); i++) {

		// The system destroys hcur by calling the DestroyCursor function. 
		// Therefore, hcur cannot be a cursor loaded using the LoadCursor function. 
		// To specify a cursor loaded from a resource, copy the cursor using 
		// the CopyCursor function, then pass the copy to SetSystemCursor.
		hBlankCursorCopy = CopyCursor(hBlankCursor);
		if (!hBlankCursorCopy) {
			uResult = GetLastError();
			lprintf_err(uResult, "HideCursors(): CopyCursor() failed, error %d\n", uResult);
			return uResult;
		}

		if (!SetSystemCursor(hBlankCursorCopy, CursorsToHide[i])) {
			uResult = GetLastError();
			lprintf_err(uResult, "HideCursors(): SetSystemCursor(%d) failed, error %d\n", CursorsToHide[i], uResult);
			return uResult;
		}
	}

	if (!DestroyCursor(hBlankCursor)) {
		uResult = GetLastError();
		lprintf_err(uResult, "HideCursors(): DestroyCursor() failed, error %d\n", uResult);
	}

	return ERROR_SUCCESS;	
}


// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(
	ULONG argc,
	PTCHAR argv[]
)
{
	ULONG uResult;


	SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

	HideCursors();

	uResult = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, " IncreaseProcessWorkingSetSize() failed, error %d\n", uResult);
		// try to continue
	}


	uResult = CheckForXenInterface();
	if (ERROR_SUCCESS != uResult) {
		Lprintf_err(uResult, "Init(): CheckForXenInterface()");
		return ERROR_NOT_SUPPORTED;
	}

	// hide console window, all logging should go to the log file (or event log)
	FreeConsole();

	// Manual reset, initial state is not signaled
	g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hStopServiceEvent) {
		uResult = GetLastError();
		Lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}
	// Manual reset, initial state is not signaled
	g_hCleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hCleanupFinishedEvent) {
		uResult = GetLastError();
		CloseHandle(g_hStopServiceEvent);
		Lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}

	SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);

	uResult = WatchForEvents();
	if (ERROR_SUCCESS != uResult) {
		Lprintf_err(uResult, "ServiceExecutionThread(): WatchForEvents()");
	}
	SetEvent(g_hCleanupFinishedEvent);

	return ERROR_SUCCESS;
}
