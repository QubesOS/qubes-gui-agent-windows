#include <windows.h>
#include "tchar.h"
#include "qubes-gui-protocol.h"
#include "libvchan.h"
#include "glue.h"
#include "log.h"
#include "qvcontrol.h"

#define lprintf_err	Lprintf_err
#define lprintf	Lprintf

HANDLE g_hStopServiceEvent;
HANDLE g_hCleanupFinishedEvent;

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

ULONG g_uScreenWidth = 0;
ULONG g_uScreenHeight = 0;

/* Get PFNs of hWnd Window from QVideo driver and prepare relevant shm_cmd
 * struct.
 */
ULONG PrepareShmCmd(
	HWND hWnd,
	struct shm_cmd ** ppShmCmd
)
{
	QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
	ULONG uResult;
	ULONG uShmCmdSize = 0;
	struct shm_cmd *pShmCmd = NULL;

	if (!ppShmCmd)
		return ERROR_INVALID_PARAMETER;
	*ppShmCmd = NULL;

	memset(&QvGetSurfaceDataResponse, 0, sizeof(QvGetSurfaceDataResponse));

	uResult = GetWindowData(hWnd, &QvGetSurfaceDataResponse);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("GetWindowData() failed with error %d\n"), uResult);
		return uResult;
	}

	_tprintf(_T(__FUNCTION__) _T("Window %dx%d, %d bpp, screen: %d\n"), QvGetSurfaceDataResponse.cx, QvGetSurfaceDataResponse.cy,
		 QvGetSurfaceDataResponse.ulBitCount, QvGetSurfaceDataResponse.bIsScreen);
	_tprintf(_T(__FUNCTION__) _T("PFNs: %d; 0x%x, 0x%x, 0x%x\n"), QvGetSurfaceDataResponse.PfnArray.uNumberOf4kPages,
		 QvGetSurfaceDataResponse.PfnArray.Pfn[0], QvGetSurfaceDataResponse.PfnArray.Pfn[1], QvGetSurfaceDataResponse.PfnArray.Pfn[2]);

	uShmCmdSize = sizeof(struct shm_cmd) + QvGetSurfaceDataResponse.PfnArray.uNumberOf4kPages * sizeof(uint32_t);

	pShmCmd = malloc(uShmCmdSize);
	if (!pShmCmd) {
		_tprintf(_T(__FUNCTION__) _T("Failed to allocate %d bytes for shm_cmd for window 0x%x\n"), uShmCmdSize, hWnd);
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	memset(pShmCmd, 0, uShmCmdSize);

	pShmCmd->shmid = 0;
	/* video buffer is iterlaced with some crap (double buffer?) so place it
	 * outside of the window as a workaround (FIXME) */
	pShmCmd->width = QvGetSurfaceDataResponse.cx * 2;
	pShmCmd->height = QvGetSurfaceDataResponse.cy;
	pShmCmd->bpp = QvGetSurfaceDataResponse.ulBitCount;
	pShmCmd->off = 0;
	pShmCmd->num_mfn = QvGetSurfaceDataResponse.PfnArray.uNumberOf4kPages;
	pShmCmd->domid = 0;

	memcpy(&pShmCmd->mfns, &QvGetSurfaceDataResponse.PfnArray.Pfn, QvGetSurfaceDataResponse.PfnArray.uNumberOf4kPages * sizeof(uint32_t));

	*ppShmCmd = pShmCmd;

	return ERROR_SUCCESS;
}

void send_pixmap_mfns(
	HWND hWnd
)
{
	ULONG uResult;
	struct shm_cmd *pShmCmd = NULL;
	struct msg_hdr hdr;
	int size;

	uResult = PrepareShmCmd(hWnd, &pShmCmd);
	if (ERROR_SUCCESS != uResult) {
		_tprintf(_T(__FUNCTION__) _T("PrepareShmCmd() failed with error %d\n"), uResult);
		return;
	}

	if (pShmCmd->num_mfn == 0 || pShmCmd->num_mfn > MAX_MFN_COUNT) {
		_tprintf(_T(__FUNCTION__) _T("got num_mfn=0x%x for window 0x%x\n"), pShmCmd->num_mfn, (int)hWnd);
		free(pShmCmd);
		return;
	}

	/* FIXME: only if QvGetSurfaceDataResponse.bIsScreen == 1 ? */
	g_uScreenWidth = pShmCmd->width;
	g_uScreenHeight = pShmCmd->height;

	size = pShmCmd->num_mfn * sizeof(uint32_t);

	hdr.type = MSG_MFNDUMP;
	hdr.window = (uint32_t) hWnd;
	hdr.untrusted_len = sizeof(struct shm_cmd) + size;
	write_struct(hdr);
	write_data(pShmCmd, sizeof(struct shm_cmd) + size);

	free(pShmCmd);
}

ULONG send_window_create(
	HWND window
)
{
	WINDOWINFO wi;
	ULONG uResult;
	struct msg_hdr hdr;
	struct msg_create mc;
	struct msg_map_info mmi;

	wi.cbSize = sizeof(wi);
	/* special case for full screen */
	if (window == NULL) {
		QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
		ULONG uResult;

		/* TODO: multiple screens? */
		wi.rcWindow.left = 0;
		wi.rcWindow.top = 0;

		memset(&QvGetSurfaceDataResponse, 0, sizeof(QvGetSurfaceDataResponse));

		uResult = GetWindowData(window, &QvGetSurfaceDataResponse);
		if (ERROR_SUCCESS != uResult) {
			_tprintf(_T(__FUNCTION__) _T("GetWindowData() failed with error %d\n"), uResult);
			return uResult;
		}

		wi.rcWindow.right = QvGetSurfaceDataResponse.cx;
		wi.rcWindow.bottom = QvGetSurfaceDataResponse.cy;
	} else 	if (!GetWindowInfo(window, &wi)) {
		uResult = GetLastError();
		lprintf_err(uResult, "send_window_create: GetWindowInfo()");
		return uResult;
	}

	hdr.type = MSG_CREATE;
	hdr.window = (uint32_t) window;

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

void handle_xconf(
)
{
	struct msg_xconf xconf;

	read_all_vchan_ext((char *)&xconf, sizeof(xconf));

	/* TODO: set video device resolution */
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
	default:
		_tprintf(_T(__FUNCTION__) _T("got unknown msg type %d, ignoring\n"), hdr.type);
	case MSG_KEYPRESS:
//              handle_keypress(g, hdr.window);
//		break;
	case MSG_CONFIGURE:
//              handle_configure(g, hdr.window);
//		break;
	case MSG_MAP:
//              handle_map(g, hdr.window);
//		break;
	case MSG_BUTTON:
//              handle_button(g, hdr.window);
//		break;
	case MSG_MOTION:
//              handle_motion(g, hdr.window);
//		break;
	case MSG_CLOSE:
//              handle_close(g, hdr.window);
//		break;
	case MSG_CROSSING:
//              handle_crossing(g, hdr.window);
//		break;
	case MSG_FOCUS:
//              handle_focus(g, hdr.window);
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
	BOOLEAN bVchanClientConnected;
	HANDLE WatchedEvents[MAXIMUM_WAIT_OBJECTS];
	HANDLE hWindowDamageEvent;
	HDC hDC;
	ULONG uDamage = 0;

	hDC = GetDC(NULL);

	hWindowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if (ERROR_SUCCESS != RegisterWatchedDC(hDC, hWindowDamageEvent)) {
		// This DC has some problems, or it has been destroyed already.
		// Skip it and delete from the array.
		ReleaseDC(NULL, hDC);
		CloseHandle(hWindowDamageEvent);

		return ERROR_INVALID_PARAMETER;
	}
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

	bVchanClientConnected = FALSE;
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
				if (bVchanClientConnected)
					send_window_damage_event(NULL, 0, 0, g_uScreenWidth, g_uScreenHeight);
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

				if (!bVchanClientConnected) {

					lprintf("WatchForEvents(): A vchan client has connected\n");

					// Remove the xenstore device/vchan/N entry.
					uResult = libvchan_server_handle_connected(ctrl);
					if (uResult) {
						lprintf_err(ERROR_INVALID_FUNCTION, "WatchForEvents(): libvchan_server_handle_connected()");
						bVchanReturnedError = TRUE;
						break;
					}

					send_protocol_version();
					handle_xconf();
					send_window_create(NULL);
					send_pixmap_mfns(NULL);

					bVchanClientConnected = TRUE;
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

	if (!bVchanClientConnected)
		// Remove the xenstore device/vchan/N entry.
		libvchan_server_handle_connected(ctrl);

	if (bVchanClientConnected)
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

	Lprintf("CtrlHandler(): Shutdown complete\n");
	ExitProcess(0);
	return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(
	ULONG argc,
	PTCHAR argv[]
)
{
	ULONG uResult;

	uResult = CheckForXenInterface();
	if (ERROR_SUCCESS != uResult) {
		Lprintf_err(uResult, "Init(): CheckForXenInterface()");
		return ERROR_NOT_SUPPORTED;
	}
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
