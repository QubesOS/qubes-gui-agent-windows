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

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

char g_DomainName[256] = "<unknown>";

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

HANDLE g_ShellEventsThread = NULL;

HWND g_DesktopWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

ULONG ProcessUpdatedWindows(BOOL bUpdateEverything, HDC screenDC);

// can be called from main thread, shell hook thread or ResetWatch thread
ULONG RemoveWatchedDC(IN OUT WATCHED_DC *watchedDC)
{
    if (!watchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("hwnd=0x%x, hdc=0x%x", watchedDC->WindowHandle, watchedDC->DC);
    free(watchedDC->PfnArray);

    if (g_VchanClientConnected)
    {
        SendWindowUnmap(watchedDC->WindowHandle);
        if (watchedDC->WindowHandle) // never destroy screen "window"
            SendWindowDestroy(watchedDC->WindowHandle);
    }

    free(watchedDC);

    return ERROR_SUCCESS;
}

ULONG StartShellEventsThread(void)
{
    DWORD threadId;

    LogVerbose("start");

    if (g_ShellEventsThread)
    {
        LogError("shell events thread already running: handle 0x%x, window 0x%x", g_ShellEventsThread, g_ShellEventsWindow);
        // this is abnormal, returning error here will cause termination
        return ERROR_ALREADY_EXISTS;
    }

    g_ShellEventsThread = CreateThread(NULL, 0, ShellEventsThread, NULL, 0, &threadId);
    if (!g_ShellEventsThread)
        return perror("CreateThread(ShellEventsThread)");

    LogInfo("shell events thread ID: 0x%x (created)", threadId);
    return ERROR_SUCCESS;
}

ULONG StopShellEventsThread(void)
{
    ULONG status;
    DWORD waitResult;

    LogVerbose("shell hook window: 0x%x", g_ShellEventsWindow);
    if (!g_ShellEventsThread)
        return ERROR_SUCCESS;

    // SendMessage waits until the message is processed
    if (!SendMessage(g_ShellEventsWindow, WM_CLOSE, 0, 0))
    {
        status = perror("PostMessage(WM_CLOSE)");
        LogWarning("Terminating shell events thread forcibly");
        TerminateThread(g_ShellEventsThread, 0);
    }

    LogVerbose("waiting for thread to exit");
    waitResult = WaitForSingleObject(g_ShellEventsThread, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LogWarning("wait failed or timed out, killing thread forcibly");
        TerminateThread(g_ShellEventsThread, 0);
    }

    CloseHandle(g_ShellEventsThread);

    g_ShellEventsThread = NULL;
    g_ShellEventsWindow = NULL;

    LogDebug("shell events thread terminated");
    return ERROR_SUCCESS;
}

// Reinitialize everything, called after a session switch.
// This is executed as another thread to avoid shell events killing itself without finishing the job.
// TODO: use this with session change notification instead of AttachToInputDesktop every time?
// NOTE: this function doesn't close/reopen qvideo's screen section
static DWORD WINAPI ResetWatch(IN void *threadParam)
{
    WATCHED_DC *watchedDC;
    WATCHED_DC *nextWatchedDC;

    LogVerbose("start");

    StopShellEventsThread();

    LogDebug("removing watches");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    watchedDC = (WATCHED_DC *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WATCHED_DC *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WATCHED_DC, ListEntry);
        nextWatchedDC = (WATCHED_DC *) watchedDC->ListEntry.Flink;

        RemoveEntryList(&watchedDC->ListEntry);
        RemoveWatchedDC(watchedDC);

        watchedDC = nextWatchedDC;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopWindow = NULL;

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
        SendWindowMap(NULL);
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
        SendWindowUnmap(NULL);
    }
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

// can be called from main thread or shell hook thread
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

    if (!watchedDC)
        return ERROR_INVALID_PARAMETER;

    LogDebug("hwnd=0x%x, hdc=0x%x", watchedDC->WindowHandle, watchedDC->DC);

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
            SendWindowMap(watchedDC);

        if (!currentlyVisible && watchedDC->IsVisible)
            SendWindowUnmap(watchedDC->WindowHandle);
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
        EnumWindows(FindModalChildProc, (LPARAM) &modalParams);
        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            WATCHED_DC *modalDc = FindWindowByHandle(modalParams.ModalWindow);
            if (modalDc && !modalDc->ModalParent)
            {
                modalDc->ModalParent = watchedDC->WindowHandle;
                SendWindowUnmap(modalDc->WindowHandle);
                SendWindowMap(modalDc);
            }
        }
    }

    watchedDC->IsVisible = currentlyVisible;

    if (IsIconic(watchedDC->WindowHandle))
    {
        if (!watchedDC->IsIconic)
        {
            LogDebug("0x%x IsIconic: minimizing", watchedDC->WindowHandle);
            SendWindowFlags(watchedDC->WindowHandle, WINDOW_FLAG_MINIMIZE, 0);
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
                SendWindowConfigure(watchedDC);

            if (damageArea == NULL)
            { // assume the whole area changed
                SendWindowDamageEvent(watchedDC->WindowHandle,
                    0,
                    0,
                    watchedDC->WindowRect.right - watchedDC->WindowRect.left,
                    watchedDC->WindowRect.bottom - watchedDC->WindowRect.top);
            }
            else
            {
                // send only intersection of damage area and window area
                IntersectRect(&intersection, damageArea, &watchedDC->WindowRect);
                SendWindowDamageEvent(watchedDC->WindowHandle,
                    intersection.left - watchedDC->WindowRect.left,
                    intersection.top - watchedDC->WindowRect.top,
                    intersection.right - watchedDC->WindowRect.left,
                    intersection.bottom - watchedDC->WindowRect.top);
            }
        }
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

BOOL ShouldAcceptWindow(IN HWND window, IN const WINDOWINFO *pwi OPTIONAL)
{
    WINDOWINFO wi;

    if (!pwi)
    {
        if (!GetWindowInfo(window, &wi))
            return FALSE;
        pwi = &wi;
    }

    //LogVerbose("0x%x: %x %x", hWnd, pwi->dwStyle, pwi->dwExStyle);
    if (!IsWindowVisible(window))
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
static BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    WINDOWINFO wi;
    BANNED_WINDOWS *bannedWindowList = (BANNED_WINDOWS *) lParam;
    ULONG i;

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hWnd, &wi))
        return TRUE;

    if (!ShouldAcceptWindow(hWnd, &wi))
        return TRUE;

    if (bannedWindowList)
    {
        for (i = 0; i < bannedWindowList->Count; i++)
        {
            if (bannedWindowList->BannedHandles[i] == hWnd)
                return TRUE;
        }
    }

    AddWindowWithInfo(hWnd, &wi);

    return TRUE;
}

// Main function that scans for window updates.
// Called after receiving damage event from qvideo.
static ULONG ProcessUpdatedWindows(IN BOOL updateEverything, IN HDC screenDC)
{
    WATCHED_DC *watchedDC;
    WATCHED_DC *nextWatchedDC;
    BYTE bannedWindowListBuffer[sizeof(BANNED_WINDOWS) * 4];
    BANNED_WINDOWS *bannedWindowList = (BANNED_WINDOWS *) &bannedWindowListBuffer;
    BOOL recheckWindows = FALSE;
    HWND oldDesktopWindow = g_DesktopWindow;
    ULONG totalPages, page, dirtyPages = 0;
    RECT dirtyArea, currentArea;
    BOOL first = TRUE;
    static HWND explorerWindow = NULL;
    static HWND taskbarWindow = NULL;
    static HWND startButtonWindow = NULL;

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

    if (!explorerWindow || recheckWindows || !IsWindow(explorerWindow))
        explorerWindow = FindWindow(NULL, L"Program Manager");

    if (!taskbarWindow || recheckWindows || !IsWindow(taskbarWindow))
    {
        taskbarWindow = FindWindow(L"Shell_TrayWnd", NULL);

        if (taskbarWindow)
        {
            if (g_SeamlessMode)
                ShowWindow(taskbarWindow, SW_HIDE);
            else
                ShowWindow(taskbarWindow, SW_SHOW);
        }
    }

    if (!startButtonWindow || recheckWindows || !IsWindow(startButtonWindow))
    {
        startButtonWindow = FindWindowEx(g_DesktopWindow, NULL, L"Button", NULL);

        if (startButtonWindow)
        {
            if (g_SeamlessMode)
                ShowWindow(startButtonWindow, SW_HIDE);
            else
                ShowWindow(startButtonWindow, SW_SHOW);
        }
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopWindow, explorerWindow, taskbarWindow, startButtonWindow);

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

    bannedWindowList->Count = 4;
    bannedWindowList->BannedHandles[0] = g_DesktopWindow;
    bannedWindowList->BannedHandles[1] = explorerWindow;
    bannedWindowList->BannedHandles[2] = taskbarWindow;
    bannedWindowList->BannedHandles[3] = startButtonWindow;

    EnterCriticalSection(&g_csWatchedWindows);

    EnumWindows(EnumWindowsProc, (LPARAM) bannedWindowList);

    watchedDC = (WATCHED_DC *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WATCHED_DC *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WATCHED_DC, ListEntry);
        nextWatchedDC = (WATCHED_DC *) watchedDC->ListEntry.Flink;

        if (!IsWindow(watchedDC->WindowHandle) || !ShouldAcceptWindow(watchedDC->WindowHandle, NULL))
        {
            RemoveEntryList(&watchedDC->ListEntry);
            RemoveWatchedDC(watchedDC);
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

// g_csWatchedWindows critical section must be entered
WATCHED_DC *AddWindowWithInfo(IN HWND hWnd, IN const WINDOWINFO *windowInfo)
{
    WATCHED_DC *watchedDC = NULL;

    if (!windowInfo)
        return NULL;

    LogDebug("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x",
        hWnd, windowInfo->rcWindow.left, windowInfo->rcWindow.top, windowInfo->rcWindow.right, windowInfo->rcWindow.bottom, windowInfo->dwStyle, windowInfo->dwExStyle);

    watchedDC = FindWindowByHandle(hWnd);
    if (watchedDC)
        // already being watched
        return watchedDC;

    if ((windowInfo->rcWindow.top - windowInfo->rcWindow.bottom == 0) || (windowInfo->rcWindow.right - windowInfo->rcWindow.left == 0))
        return NULL;

    watchedDC = (WATCHED_DC *) malloc(sizeof(WATCHED_DC));
    if (!watchedDC)
        return NULL;

    ZeroMemory(watchedDC, sizeof(WATCHED_DC));

    watchedDC->IsVisible = IsWindowVisible(hWnd);
    watchedDC->IsIconic = IsIconic(hWnd);

    watchedDC->IsStyleChecked = FALSE;
    watchedDC->TimeAdded = watchedDC->TimeModalChecked = GetTickCount();

    LogDebug("0x%x: visible=%d, iconic=%d", watchedDC->WindowHandle, watchedDC->IsVisible, watchedDC->IsIconic);

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (windowInfo->rcWindow.right - windowInfo->rcWindow.left == g_ScreenWidth
        && windowInfo->rcWindow.bottom - windowInfo->rcWindow.top == g_ScreenHeight)
    {
        LogDebug("popup too large: %dx%d, screen %dx%d",
            windowInfo->rcWindow.right - windowInfo->rcWindow.left,
            windowInfo->rcWindow.bottom - windowInfo->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
        watchedDC->IsOverrideRedirect = FALSE;
    }
    else
    {
        // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
        if ((windowInfo->dwStyle & WS_CAPTION) == WS_CAPTION) // normal window
            watchedDC->IsOverrideRedirect = FALSE;
        else if (((windowInfo->dwStyle & WS_SYSMENU) == WS_SYSMENU) && ((windowInfo->dwExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
            // Metro apps without WS_CAPTION.
            // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
            // but I guess MS doesn't adhere to its own standards...
            watchedDC->IsOverrideRedirect = FALSE;
        else
            watchedDC->IsOverrideRedirect = TRUE;
    }

    if (watchedDC->IsOverrideRedirect)
    {
        LogDebug("popup: %dx%d, screen %dx%d",
            windowInfo->rcWindow.right - windowInfo->rcWindow.left,
            windowInfo->rcWindow.bottom - windowInfo->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
    }

    watchedDC->WindowHandle = hWnd;
    watchedDC->WindowRect = windowInfo->rcWindow;

    watchedDC->PfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));

    watchedDC->MaxWidth = g_ScreenWidth;
    watchedDC->MaxHeight = g_ScreenHeight;

    if (g_VchanClientConnected)
    {
        SendWindowCreate(watchedDC);
        SendWindowName(hWnd);
    }

    InsertTailList(&g_WatchedWindowsList, &watchedDC->ListEntry);

    return watchedDC;
}

// main event loop
// TODO: refactor into smaller parts
static ULONG WINAPI WatchForEvents(void)
{
    HANDLE vchan, mailslot;
    OVERLAPPED olVchan, olMailslot;
    unsigned int firedPort;
    ULONG eventCount;
    DWORD i, signaledEvent, size;
    BOOL vchanIoInProgress;
    ULONG status;
    BOOL exitLoop;
    HANDLE watchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE windowDamageEvent, fullScreenOnEvent, fullScreenOffEvent;
    HDC screenDC;
    ULONG damageNumber = 0;
    struct shm_cmd *shmCmd = NULL;
    QH_MESSAGE qhm;
    HANDLE hookServerProcess;
    HANDLE hookShutdownEvent;

    LogDebug("start");
    windowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

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
    vchanIoInProgress = FALSE;
    exitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize());

    while (TRUE)
    {
        eventCount = 0;

        // Order matters.
        watchedEvents[eventCount++] = g_ShutdownEvent;
        watchedEvents[eventCount++] = windowDamageEvent;
        watchedEvents[eventCount++] = fullScreenOnEvent;
        watchedEvents[eventCount++] = fullScreenOffEvent;
        watchedEvents[eventCount++] = g_ResolutionChangeEvent;
        watchedEvents[eventCount++] = olMailslot.hEvent;

        status = ERROR_SUCCESS;

        VchanPrepareToSelect();
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!vchanIoInProgress && !ReadFile(vchan, &firedPort, 1, NULL, &olVchan))
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

        watchedEvents[eventCount++] = olVchan.hEvent;

        // Start hook maislot async read.
        // Even if there is data available right away, processing is done in the event handler.
        status = ReadFile(mailslot, &qhm, sizeof(qhm), NULL, &olMailslot);

        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            status = perror("WaitForMultipleObjects");
            break;
        }
        else
        {
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
                ChangeResolution(&screenDC, windowDamageEvent);
                break;

            case 5: // mailslot read: message from our gui hook
                if (!GetOverlappedResult(mailslot, &olMailslot, &size, FALSE))
                {
                    perror("GetOverlappedResult(mailslot)");
                    exitLoop = TRUE;
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

                    vchanIoInProgress = FALSE;

                    LogInfo("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    if (!VchanIsServerConnected())
                    {
                        LogError("VchanIsServerConnected() failed");
                        exitLoop = TRUE;
                        break;
                    }

                    SendProtocolVersion();

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
                        perror("RegisterWatchedDC");
                        exitLoop = TRUE;
                        break;
                    }

                    // send the whole screen framebuffer map
                    SendWindowCreate(NULL);
                    SendWindowMfns(NULL);

                    if (!g_SeamlessMode)
                    {
                        LogInfo("init in fullscreen mode");
                        SendWindowMap(NULL);
                    }
                    else
                    {
                        if (ERROR_SUCCESS != StartShellEventsThread())
                        {
                            LogError("StartShellEventsThread failed, exiting");
                            exitLoop = TRUE;
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

                while (VchanGetReadBufferSize())
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
            WaitForSingleObject(olVchan.hEvent, INFINITE);
        }
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
    CloseHandle(windowDamageEvent);

    StopShellEventsThread();
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
