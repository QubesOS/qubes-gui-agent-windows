#define OEMRESOURCE

#include <windows.h>
#include <aclapi.h>
#include <winsock2.h>

#include "vchan.h"
#include "qvcontrol.h"
#include "resolution.h"
#include "shell_events.h"
#include "resource.h"
#include "log.h"
#include "send.h"
#include "handlers.h"

#define FULLSCREEN_ON_EVENT_NAME L"WGA_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"WGA_FULLSCREEN_OFF"

// If set, only invalidate parts of the screen that changed according to
// qvideo's dirty page scan of surface memory buffer.
BOOL g_bUseDirtyBits;

LONG g_ScreenHeight;
LONG g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;

BOOL g_bFullScreenMode = FALSE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;


char g_HostName[256] = "<unknown>";


static HANDLE CreateNamedEvent(WCHAR *name)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    EXPLICIT_ACCESS ea = {0};
    PACL acl = NULL;
    HANDLE event = NULL;

    // we're running as SYSTEM at the start, default ACL for new objects is too restrictive
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfAccessPermissions = EVENT_MODIFY_STATE|READ_CONTROL;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.ptstrName = L"EVERYONE";

    if (SetEntriesInAcl(1, &ea, NULL, &acl) != ERROR_SUCCESS)
    {
        perror("SetEntriesInAcl");
        goto cleanup;
    }
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
    {
        perror("InitializeSecurityDescriptor");
        goto cleanup;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE))
    {
        perror("SetSecurityDescriptorDacl");
        goto cleanup;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // autoreset, not signaled
    event = CreateEvent(&sa, FALSE, FALSE, name);

    if (!event)
    {
        perror("CreateEvent");
        goto cleanup;
    }

cleanup:
    if (acl)
        LocalFree(acl);
    return event;
}

static void SetFullscreenMode(void)
{
    logf("Full screen mode changed to %d", g_bFullScreenMode);

    // ResetWatch kills the shell event thread and removes all watched windows.
    // If fullscreen is off the shell event thread is also restarted.
    ResetWatch(NULL);

    if (g_bFullScreenMode)
    {
        // show the screen window
        send_window_map(NULL);
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            logf("Changing resolution to match host's");
            RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, 32, 0, 0);
        }
        // hide the screen window
        send_window_unmap(NULL);
    }
}

static ULONG WINAPI WatchForEvents(void)
{
    HANDLE vchan;
    OVERLAPPED ol;
    unsigned int fired_port;
    ULONG uEventNumber;
    DWORD i, dwSignaledEvent;
    BOOL bVchanIoInProgress;
    ULONG uResult;
    BOOL bExitLoop;
    HANDLE WatchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE hWindowDamageEvent;
    HANDLE hFullScreenOnEvent;
    HANDLE hFullScreenOffEvent;
    HANDLE hShutdownEvent;
    HDC hDC;
    ULONG uDamage = 0;
    struct shm_cmd *pShmCmd = NULL;

    debugf("start");
    hWindowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    if (!VchanInitServer(6000))
    {
        errorf("VchanInitServer() failed");
        return GetLastError();
    }

    logf("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize());

    vchan = VchanGetHandle();

    ZeroMemory(&ol, sizeof(ol));
    ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    hShutdownEvent = CreateNamedEvent(WGA_SHUTDOWN_EVENT_NAME);
    if (!hShutdownEvent)
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

    g_VchanClientConnected = FALSE;
    bVchanIoInProgress = FALSE;
    bExitLoop = FALSE;

    while (TRUE)
    {
        uEventNumber = 0;

        // Order matters.
        WatchedEvents[uEventNumber++] = hShutdownEvent;
        WatchedEvents[uEventNumber++] = hWindowDamageEvent;
        WatchedEvents[uEventNumber++] = hFullScreenOnEvent;
        WatchedEvents[uEventNumber++] = hFullScreenOffEvent;
        WatchedEvents[uEventNumber++] = g_ResolutionChangeEvent;

        uResult = ERROR_SUCCESS;

        VchanPrepareToSelect();
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!bVchanIoInProgress && !ReadFile(vchan, &fired_port, 1, NULL, &ol))
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

        WatchedEvents[uEventNumber++] = ol.hEvent;

        dwSignaledEvent = WaitForMultipleObjects(uEventNumber, WatchedEvents, FALSE, INFINITE);
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
                break;
            }

            //debugf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
            switch (dwSignaledEvent)
            {
            case 1: // damage event

                debugf("Damage %d\n", uDamage++);

                if (g_VchanClientConnected)
                {
                    ProcessUpdatedWindows(TRUE);
                }
                break;

            case 2: // fullscreen on event
                if (g_bFullScreenMode)
                    break; // already in fullscreen
                g_bFullScreenMode = TRUE;
                SetFullscreenMode();
                break;

            case 3: // fullscreen off event
                if (!g_bFullScreenMode)
                    break; // already in seamless
                g_bFullScreenMode = FALSE;
                SetFullscreenMode();
                break;

            case 4: // resolution change event, signaled by ResolutionChangeThread
                // Params are in g_ResolutionChangeParams
                ChangeResolution(&hDC, hWindowDamageEvent);
                break;

            case 5: // vchan receive
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

                    logf("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    if (!VchanIsServerConnected())
                    {
                        errorf("libvchan_server_handle_connected() failed");
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
                    hDC = GetDC(NULL);
                    uResult = RegisterWatchedDC(hDC, hWindowDamageEvent);
                    if (ERROR_SUCCESS != uResult)
                    {
                        perror("RegisterWatchedDC");
                        bExitLoop = TRUE;
                        break;
                    }

                    // send the whole screen framebuffer map
                    send_window_create(NULL);
                    send_pixmap_mfns(NULL);

                    if (g_bFullScreenMode)
                    {
                        debugf("init in fullscreen mode");
                        send_window_map(NULL);
                    }
                    else
                        if (ERROR_SUCCESS != StartShellEventsThread())
                        {
                            errorf("StartShellEventsThread failed, exiting");
                            bExitLoop = TRUE;
                            break;
                        }

                    g_VchanClientConnected = TRUE;
                    break;
                }

                if (!GetOverlappedResult(vchan, &ol, &i, FALSE))
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
                        errorf("handle_server_data() failed: 0x%x", uResult);
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

    logf("main loop finished");

    if (bVchanIoInProgress)
        if (CancelIo(vchan))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(ol.hEvent, INFINITE);
        }

    if (!g_VchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        VchanIsServerConnected();
    }

    if (g_VchanClientConnected)
        VchanClose();

    CloseHandle(ol.hEvent);
    CloseHandle(hWindowDamageEvent);

    StopShellEventsThread();
    UnregisterWatchedDC(hDC);
    CloseScreenSection();
    ReleaseDC(NULL, hDC);
    logf("exiting");

    return bExitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static ULONG IncreaseProcessWorkingSetSize(SIZE_T uNewMinimumWorkingSetSize, SIZE_T uNewMaximumWorkingSetSize)
{
    SIZE_T uMinimumWorkingSetSize = 0;
    SIZE_T uMaximumWorkingSetSize = 0;

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize))
        return perror("GetProcessWorkingSetSize");

    if (!SetProcessWorkingSetSize(GetCurrentProcess(), uNewMinimumWorkingSetSize, uNewMaximumWorkingSetSize))
        return perror("SetProcessWorkingSetSize");

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize))
        return perror("GetProcessWorkingSetSize");

    logf("New working set size: %d pages\n", uMaximumWorkingSetSize >> 12);

    return ERROR_SUCCESS;
}

ULONG HideCursors(void)
{
    HCURSOR	hBlankCursor;
    HCURSOR	hBlankCursorCopy;
    UCHAR i;
    ULONG CursorsToHide[] = {
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

    debugf("start");
    hBlankCursor = LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_BLANK), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE);
    if (!hBlankCursor)
        return perror("LoadImage");

    for (i = 0; i < RTL_NUMBER_OF(CursorsToHide); i++)
    {
        // The system destroys hcur by calling the DestroyCursor function.
        // Therefore, hcur cannot be a cursor loaded using the LoadCursor function.
        // To specify a cursor loaded from a resource, copy the cursor using
        // the CopyCursor function, then pass the copy to SetSystemCursor.
        hBlankCursorCopy = CopyCursor(hBlankCursor);
        if (!hBlankCursorCopy)
            return perror("CopyCursor");

        if (!SetSystemCursor(hBlankCursorCopy, CursorsToHide[i]))
            return perror("SetSystemCursor");
    }

    if (!DestroyCursor(hBlankCursor))
        return perror("DestroyCursor");

    return ERROR_SUCCESS;
}

ULONG DisableEffects(void)
{
    ANIMATIONINFO AnimationInfo;

    debugf("start");
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)FALSE, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETDROPSHADOW)");

    AnimationInfo.cbSize = sizeof(AnimationInfo);
    AnimationInfo.iMinAnimate = FALSE;

    if (!SystemParametersInfo(SPI_SETANIMATION, sizeof(AnimationInfo), &AnimationInfo, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETANIMATION)");

    return ERROR_SUCCESS;
}

ULONG ReadRegistryConfig(void)
{
    HKEY key = NULL;
    DWORD status = ERROR_SUCCESS;
    DWORD type;
    DWORD useDirtyBits;
    DWORD size;
    WCHAR logPath[MAX_PATH];

    // first, read the log directory
    SetLastError(status = RegOpenKey(HKEY_LOCAL_MACHINE, REG_CONFIG_KEY, &key));
    if (status != ERROR_SUCCESS)
    {
        // failed, use some safe default
        // todo: use event log
        log_init(L"c:\\", L"gui-agent");
        logf("registry config: '%s'", REG_CONFIG_KEY);
        return perror("RegOpenKey");
    }

    size = sizeof(logPath) - sizeof(TCHAR);
    RtlZeroMemory(logPath, sizeof(logPath));
    SetLastError(status = RegQueryValueEx(key, REG_CONFIG_LOG_VALUE, NULL, &type, (PBYTE)logPath, &size));
    if (status != ERROR_SUCCESS)
    {
        log_init(L"c:\\", L"gui-agent");
        errorf("Failed to read log path from '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_LOG_VALUE);
        perror("RegQueryValueEx");
        status = ERROR_SUCCESS; // don't fail
        goto cleanup;
    }

    if (type != REG_SZ)
    {
        log_init(L"c:\\", L"gui-agent");
        errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", REG_CONFIG_LOG_VALUE, type);
        status = ERROR_SUCCESS; // don't fail
        goto cleanup;
    }

    log_init(logPath, L"gui-agent");

    // read the rest
    size = sizeof(useDirtyBits);
    logf("reading registry value '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_DIRTY_VALUE);
    SetLastError(status = RegQueryValueEx(key, REG_CONFIG_DIRTY_VALUE, NULL, &type, (PBYTE)&useDirtyBits, &size));
    if (status != ERROR_SUCCESS)
    {
        perror("RegQueryValueEx");
        status = ERROR_SUCCESS; // don't fail, just use default value
        goto cleanup;
    }

    if (type != REG_DWORD)
    {
        errorf("Invalid type of config value '%s', 0x%x instead of REG_DWORD", REG_CONFIG_DIRTY_VALUE, type);
        status = ERROR_SUCCESS; // don't fail, just use default value
        goto cleanup;
    }

    g_bUseDirtyBits = (BOOL) useDirtyBits;

cleanup:
    if (key)
        RegCloseKey(key);

    logf("Use dirty bits? %d", g_bUseDirtyBits);
    return status;
}

static ULONG Init(void)
{
    ULONG uResult;
    WSADATA wsaData;

    if (ERROR_SUCCESS != ReadRegistryConfig())
        return GetLastError();

    debugf("start");
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

    uResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (uResult == 0)
    {
        if (0 != gethostname(g_HostName, sizeof(g_HostName)))
        {
            errorf("gethostname failed: 0x%x", uResult);
        }
        WSACleanup();
    }
    else
    {
        errorf("WSAStartup failed: 0x%x", uResult);
        // this is not fatal, only used to get host name for full desktop window title
    }

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

    logf("exiting");
    return ERROR_SUCCESS;
}
