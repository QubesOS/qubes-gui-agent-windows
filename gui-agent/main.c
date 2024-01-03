/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <dwmapi.h>
#include <Psapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "capture.h"
#include "common.h"
#include "main.h"
#include "vchan.h"
#include "resolution.h"
#include "send.h"
#include "vchan-handlers.h"
#include "util.h"

// windows-utils
#include <log.h>
#include <config.h>
#include <qubesdb-client.h>

#include <strsafe.h>

#define FULLSCREEN_ON_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_OFF"

extern struct libvchan *g_Vchan;

LONG g_ScreenHeight;
LONG g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;
BOOL g_SeamlessMode = TRUE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

char g_DomainName[256] = "<unknown>";
USHORT g_GuiDomainId = 0;

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;
BANNED_WINDOWS g_bannedWindows = { 0 };

HWND g_DesktopWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

#if defined(_DEBUG) || defined(DEBUG)
// diagnostic: dump all watched windows
void DumpWindows(void)
{
    WINDOW_DATA *entry;
    WCHAR exePath[MAX_PATH];
    DWORD pid = 0;
    HANDLE process;

    EnterCriticalSection(&g_csWatchedWindows);
    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;

    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        exePath[0] = 0;
        GetWindowThreadProcessId(entry->WindowHandle, &pid);
        if (pid)
        {
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process)
            {
                GetProcessImageFileNameW(process, exePath, RTL_NUMBER_OF(exePath));
                CloseHandle(process);
            }
        }

        LogDebug("%8x: (%6d,%6d) %4dx%4d ico=%d ovr=%d [%s] '%s' {%s}",
                 entry->WindowHandle, entry->X, entry->Y, entry->Width, entry->Height,
                 entry->IsIconic, entry->IsOverrideRedirect,
                 entry->Class, entry->Caption, exePath);

        entry = (WINDOW_DATA *)entry->ListEntry.Flink;
    }

    LeaveCriticalSection(&g_csWatchedWindows);
}
#endif

// When DWM compositing is enabled (normally always on), most windows are actually smaller
// than their size reported by winuser functions. This is because their edges contain
// invisible grip handles managed by DWM. This function returns actual visible window size.
ULONG GetWindowRectFromDwm(IN HWND window, OUT RECT* rect)
{
    RECT dwmRect;
    // get real rect of the window as managed by DWM
    HRESULT hresult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &dwmRect, sizeof(RECT));
    if (hresult != S_OK)
        return hresult;

    // TODO: remove excessive logging after confirming this works correctly
    LogVerbose("%x: DWM rect (%d,%d)-(%d,%d)", window, dwmRect.left, dwmRect.top, dwmRect.right, dwmRect.bottom);

    // monitor info is needed to adjust for DPI scaling
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX monInfo;
    monInfo.cbSize = sizeof(monInfo);
#pragma warning(push)
#pragma warning(disable:4133) // incompatible types - from 'MONITORINFOEX *' to 'LPMONITORINFO' (the function accepts both)
    if (!GetMonitorInfo(monitor, &monInfo))
        return win_perror("GetMonitorInfo failed");
#pragma warning(pop)

    LogVerbose("monitor name: %s", monInfo.szDevice);
    LogVerbose("monitor rect: (%d,%d)-(%d,%d)", monInfo.rcMonitor.left, monInfo.rcMonitor.top, monInfo.rcMonitor.right, monInfo.rcMonitor.bottom);
    LogVerbose("work rect: (%d,%d)-(%d,%d)", monInfo.rcWork.left, monInfo.rcWork.top, monInfo.rcWork.right, monInfo.rcWork.bottom);

    DEVMODE devMode;
    devMode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(monInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode);

    // adjust for DPI scaling
    double scale = (monInfo.rcMonitor.right - monInfo.rcMonitor.left) / (double)devMode.dmPelsWidth;
    LogVerbose("Pel width: %d, scale: %lf", devMode.dmPelsWidth, scale);

    rect->left = (LONG)((dwmRect.left - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->right = (LONG)((dwmRect.right - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->top = (LONG)((dwmRect.top - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    rect->bottom = (LONG)((dwmRect.bottom - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    LogVerbose("final rect: (%d,%d)-(%d,%d)", rect->left, rect->top, rect->right, rect->bottom);

    return ERROR_SUCCESS;
}

// watched windows list critical section must be entered
// Returns ERROR_SUCCESS if the window was added OR ignored (windowEntry is NULL if ignored).
// Other errors mean fatal conditions.
ULONG AddWindowWithInfo(IN HWND window, IN const WINDOWINFO *windowInfo, OUT WINDOW_DATA **windowEntry OPTIONAL)
{
    WINDOW_DATA *entry = NULL;
    ULONG status;

    if (!windowInfo)
        return ERROR_INVALID_PARAMETER;

    RECT dwmRect = windowInfo->rcWindow;
    // TODO: this calls should always succeed so we don't need to get the window size by winuser functions earlier
    status = GetWindowRectFromDwm(window, &dwmRect);
    if (!SUCCEEDED(status))
        LogWarning("GetWindowRectFromDwm failed");

    LogDebug("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x, visible=%d",
             window, dwmRect.left, dwmRect.top, dwmRect.right, dwmRect.bottom,
             windowInfo->dwStyle, windowInfo->dwExStyle, IsWindowVisible(window));

    entry = FindWindowByHandle(window);
    if (entry) // already in list
    {
        if (windowEntry)
            *windowEntry = entry;
        return ERROR_SUCCESS;
    }

    entry = (WINDOW_DATA *)malloc(sizeof(WINDOW_DATA));
    if (!entry)
    {
        LogError("Failed to malloc entry");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    ZeroMemory(entry, sizeof(WINDOW_DATA));

    entry->WindowHandle = window;
    entry->X = dwmRect.left;
    entry->Y = dwmRect.top;
    entry->Width = dwmRect.right - dwmRect.left;
    entry->Height = dwmRect.bottom - dwmRect.top;
    entry->IsIconic = IsIconic(window);
    GetWindowText(window, entry->Caption, RTL_NUMBER_OF(entry->Caption)); // don't really care about errors here
    GetClassName(window, entry->Class, RTL_NUMBER_OF(entry->Class));

    LogDebug("0x%x: iconic=%d", entry->WindowHandle, entry->IsIconic);

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (entry->Width == g_ScreenWidth && entry->Height == g_ScreenHeight)
    {
        LogDebug("popup too large: %dx%d, screen %dx%d",
                 entry->Width, entry->Height, g_ScreenWidth, g_ScreenHeight);
        entry->IsOverrideRedirect = FALSE;
    }
    else
    {
        // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
        if ((windowInfo->dwStyle & WS_CAPTION) == WS_CAPTION)
        {
            // normal window
            entry->IsOverrideRedirect = FALSE;
        }
        else if (((windowInfo->dwStyle & WS_SYSMENU) == WS_SYSMENU) && ((windowInfo->dwExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
        {
            // Metro apps without WS_CAPTION.
            // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
            // but I guess MS doesn't adhere to its own standards...
            entry->IsOverrideRedirect = FALSE;
        }
        else
            entry->IsOverrideRedirect = TRUE;
    }

    if (entry->IsOverrideRedirect)
    {
        LogDebug("popup: %dx%d, screen %dx%d",
            dwmRect.right - dwmRect.left,
            dwmRect.bottom - dwmRect.top,
            g_ScreenWidth, g_ScreenHeight);
    }

    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // send window creation info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowCreate");

        // map (show) the window if it's visible and not minimized
        if (!entry->IsIconic)
        {
            status = SendWindowMap(entry);
            if (ERROR_SUCCESS != status)
                return win_perror2(status, "SendWindowMap");
        }

        status = SendWindowName(window, entry->Caption);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowName");
    }

    if (windowEntry)
        *windowEntry = entry;
    return ERROR_SUCCESS;
}

// Remove window from the list and free memory.
// Watched windows list critical section must be entered.
ULONG RemoveWindow(IN OUT WINDOW_DATA *entry)
{
    ULONG status;

    if (!entry)
        return ERROR_INVALID_PARAMETER;

    LogDebug("0x%x", entry->WindowHandle);

    RemoveEntryList(&entry->ListEntry);

    if (g_VchanClientConnected)
    {
        status = SendWindowUnmap(entry->WindowHandle);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowUnmap");

        if (entry->WindowHandle) // never destroy screen "window"
        {
            status = SendWindowDestroy(entry->WindowHandle);
            if (ERROR_SUCCESS != status)
                return win_perror2(status, "SendWindowDestroy");
        }
    }

    free(entry);

    return ERROR_SUCCESS;
}

// EnumWindows callback for adding all top-level windows to the list.
static BOOL CALLBACK AddWindowsProc(IN HWND window, IN LPARAM lParam)
{
    WINDOWINFO wi = { 0 };
    ULONG status;

    LogVerbose("window %x", window);

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(window, &wi))
    {
        win_perror("GetWindowInfo");
        LogWarning("Skipping window %x", window);
        return TRUE;
    }

    if (!ShouldAcceptWindow(window, &wi))
        return TRUE; // skip

    status = AddWindowWithInfo(window, &wi, NULL);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "AddWindowWithInfo");
        return FALSE; // stop enumeration, fatal error occurred (should probably exit process at this point)
    }

    return TRUE;
}

// Adds all top-level windows to the watched list.
// watched windows critical section must be entered
static ULONG AddAllWindows(void)
{
    LogVerbose("start");

    // Check for special windows that should be ignored/hidden in seamless mode.
    g_bannedWindows.Explorer = FindWindow(L"Progman", L"Program Manager");

    g_bannedWindows.Taskbar = FindWindow(L"Shell_TrayWnd", NULL);
    if (g_bannedWindows.Taskbar)
    {
        ShowWindow(g_bannedWindows.Taskbar, SW_HIDE);
    }

    g_bannedWindows.Start = FindWindowEx(g_DesktopWindow, NULL, L"Button", NULL);
    if (g_bannedWindows.Start)
    {
        ShowWindow(g_bannedWindows.Start, SW_HIDE);
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
             g_DesktopWindow, g_bannedWindows.Explorer, g_bannedWindows.Taskbar, g_bannedWindows.Start);

    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, 0))
        return win_perror("EnumWindows");

    return ERROR_SUCCESS;
}

// Reinitialize watched windows, called after a seamless/fullscreen switch or resolution change.
static ULONG ResetWatch(BOOL seamlessMode)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    ULONG status;

    LogVerbose("start");

    LogDebug("removing all windows");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA *)entry->ListEntry.Flink;

        status = RemoveWindow(entry);
        if (ERROR_SUCCESS != status)
        {
            LeaveCriticalSection(&g_csWatchedWindows);
            return win_perror2(status, "RemoveWindow");
        }

        entry = nextEntry;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopWindow = NULL;
    status = ERROR_SUCCESS;

    // WatchForEvents will map the whole screen as one window.
    if (seamlessMode)
    {
        // Add all eligible windows to watch list.
        // Since this is a switch from fullscreen, no windows were watched.
        EnterCriticalSection(&g_csWatchedWindows);
        status = AddAllWindows();
        LeaveCriticalSection(&g_csWatchedWindows);
    }
    else
    {
        if (g_bannedWindows.Taskbar)
            ShowWindow(g_bannedWindows.Taskbar, SW_SHOW);

        if (g_bannedWindows.Start)
            ShowWindow(g_bannedWindows.Start, SW_SHOW);
    }

    LogVerbose("success");
    return status;
}

// set fullscreen/seamless mode
ULONG SetSeamlessMode(IN BOOL seamlessMode, IN BOOL forceUpdate)
{
    ULONG status;

    LogVerbose("Seamless mode changing to %d", seamlessMode);

    if (g_SeamlessMode == seamlessMode && !forceUpdate)
        return ERROR_SUCCESS; // nothing to do

    CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, seamlessMode, NULL);

    if (!seamlessMode)
    {
        // show the screen window
        status = SendWindowMap(NULL);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowMap(NULL)");
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            LogDebug("Changing resolution to match host's");
            RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, 0, 0);
            // FIXME: wait until the resolution actually changes?
        }
        // hide the screen window
        status = SendWindowUnmap(NULL);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowUnmap(NULL)");
    }

    // ResetWatch removes all watched windows.
    // If seamless mode is on, top-level windows are added to watch list.
    status = ResetWatch(seamlessMode);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "ResetWatch");

    g_SeamlessMode = seamlessMode;

    LogInfo("Seamless mode changed to %d", seamlessMode);

    return ERROR_SUCCESS;
}

WINDOW_DATA *FindWindowByHandle(IN HWND window)
{
    WINDOW_DATA *watchedDC;

    LogVerbose("%x", window);
    watchedDC = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (watchedDC != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WINDOW_DATA, ListEntry);

        if (window == watchedDC->WindowHandle)
            return watchedDC;

        watchedDC = (WINDOW_DATA *)watchedDC->ListEntry.Flink;
    }

    return NULL;
}

BOOL ShouldAcceptWindow(IN HWND window, IN const WINDOWINFO *windowInfo OPTIONAL)
{
    WINDOWINFO wi;

    if (!IsWindow(window))
        return FALSE;

    if (!windowInfo)
    {
        if (!GetWindowInfo(window, &wi))
        {
            win_perror("GetWindowInfo");
            return FALSE;
        }
        windowInfo = &wi;
    }

    if (g_bannedWindows.Explorer == window ||
        g_bannedWindows.Desktop == window ||
        g_bannedWindows.Start == window ||
        g_bannedWindows.Taskbar == window
        )
        return FALSE;

    // empty window rectangle? ignore (guid rejects those)
    if ((windowInfo->rcWindow.bottom - windowInfo->rcWindow.top == 0) || (windowInfo->rcWindow.right - windowInfo->rcWindow.left == 0))
    {
        LogVerbose("window rectangle is empty");
        return FALSE;
    }

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (windowInfo->dwStyle & WS_CHILD)
        return FALSE;

    // Skip invisible windows.
    if (!IsWindowVisible(window))
        return FALSE;

    // Office 2013 uses this style for some helper windows that are drawn on/near its border.
    // 0x800 exstyle is undocumented...
    // FIXME: ignoring these border "windows" causes weird window looks.
    // Investigate why moving main Office window doesn't move these windows.
    if (windowInfo->dwExStyle == (WS_EX_LAYERED | WS_EX_TOOLWINDOW | 0x800))
        return FALSE;

    // undocumented styles, seem to be used by helper windows that have "visible" style but really aren't
    // TODO more robust detection
    if (windowInfo->dwExStyle & 0xc0000000)
        return FALSE;

    return TRUE;
}

// Enumerate top-level windows, searching for one that is modal
// in relation to a parent one (passed in lParam).
static BOOL WINAPI FindModalChildProc(IN HWND hwnd, IN LPARAM lParam)
{
    MODAL_SEARCH_PARAMS *msp = (MODAL_SEARCH_PARAMS *)lParam;
    LONG wantedStyle = WS_POPUP | WS_VISIBLE;
    HWND owner = GetWindow(hwnd, GW_OWNER);

    // Modal windows are not child windows but owned windows.
    if (owner != msp->ParentWindow)
        return TRUE;

    if ((GetWindowLong(hwnd, GWL_STYLE) & wantedStyle) != wantedStyle)
        return TRUE;

    msp->ModalWindow = hwnd;
    LogVerbose("0x%x: seems OK for 0x%x", hwnd, msp->ParentWindow);
    return FALSE; // stop enumeration
}

// Refresh data about a window, send notifications to guid if needed.
static ULONG UpdateWindowData(IN OUT WINDOW_DATA *wd, OUT BOOL *skip)
{
    ULONG status;
    WINDOWINFO wi = { 0 };
    int x, y, width, height;
    BOOL updateNeeded;
    WCHAR caption[256];
    MODAL_SEARCH_PARAMS modalParams = { 0 };

    *skip = FALSE;

    // get current window state
    wi.cbSize = sizeof(wi);

    if (!GetWindowInfo(wd->WindowHandle, &wi) || !ShouldAcceptWindow(wd->WindowHandle, &wi))
    {
        LogDebug("window %x disappeared, removing", wd->WindowHandle);
        status = RemoveWindow(wd);
        *skip = TRUE;
        return status;
    }

    // iconic state
    if (IsIconic(wd->WindowHandle))
    {
        if (!wd->IsIconic)
        {
            LogDebug("minimizing %x", wd->WindowHandle);
            wd->IsIconic = TRUE;
            *skip = TRUE;
            return SendWindowFlags(wd->WindowHandle, WINDOW_FLAG_MINIMIZE, 0); // exit
        }
        return ERROR_SUCCESS;
    }
    else
    {
        LogVerbose("%x not iconic", wd->WindowHandle);
        wd->IsIconic = FALSE;
    }

    // coords
    x = wi.rcWindow.left;
    y = wi.rcWindow.top;
    width = wi.rcWindow.right - wi.rcWindow.left;
    height = wi.rcWindow.bottom - wi.rcWindow.top;

    updateNeeded = (x != wd->X || y != wd->Y || width != wd->Width || height != wd->Height);

    wd->X = x;
    wd->Y = y;
    wd->Width = width;
    wd->Height = height;
    LogVerbose("%x (%d,%d) %dx%d", wd->WindowHandle, x, y, width, height);

    if (updateNeeded)
        SendWindowConfigure(wd);

    // visibility
    if (!IsWindowVisible(wd->WindowHandle))
    {
        LogDebug("window %x turned invisible", wd->WindowHandle);
        status = RemoveWindow(wd);
        *skip = TRUE;
        return status;
    }

    // caption
    GetWindowText(wd->WindowHandle, caption, RTL_NUMBER_OF(caption));
    if (0 != wcscmp(wd->Caption, caption))
    {
        // caption changed
        StringCchCopy(wd->Caption, RTL_NUMBER_OF(wd->Caption), caption);
        SendWindowName(wd->WindowHandle, wd->Caption);
    }

    // style
    if (wi.dwStyle & WS_DISABLED)
    {
        // possibly showing a modal window
        LogDebug("0x%x is WS_DISABLED, searching for modal window", wd->WindowHandle);
        modalParams.ParentWindow = wd->WindowHandle;
        modalParams.ModalWindow = NULL;

        // No checking for success, EnumWindows returns FALSE if the callback function returns FALSE.
        EnumWindows(FindModalChildProc, (LPARAM)&modalParams);

        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            WINDOW_DATA *modalWindow = FindWindowByHandle(modalParams.ModalWindow);
            if (modalWindow && !modalWindow->ModalParent)
            {
                // need to toggle map since this is the only way to change modal status for gui daemon
                modalWindow->ModalParent = wd->WindowHandle;
                status = SendWindowUnmap(modalWindow->WindowHandle);
                if (ERROR_SUCCESS != status)
                    return win_perror2(status, "SendWindowUnmap");

                status = SendWindowMap(modalWindow);
                if (ERROR_SUCCESS != status)
                    return win_perror2(status, "SendWindowMap");
            }
        }
    }

    return ERROR_SUCCESS;
}

// Called after receiving new frame.
static ULONG ProcessUpdatedWindows(IN const CAPTURE_FRAME* frame)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    HWND oldDesktopWindow = g_DesktopWindow;
    BOOL first = TRUE;
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start");
    if (!g_SeamlessMode)
    {
        if (frame->dirty_rects_count == 0)
        {
            // normally we don't get frames with 0 dirty rects unless it's the 1st one
            // then refresh everything
            LogVerbose("no dirty rects, updating whole screen");
            SendWindowDamageEvent(NULL, 0, 0, g_ScreenWidth, g_ScreenHeight);
        }
        else
        {
            for (UINT i = 0; i < frame->dirty_rects_count; i++)
            {
                RECT rect = frame->dirty_rects[i];
                SendWindowDamageEvent(NULL, rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top);
            }
        }

        return ERROR_SUCCESS;
    }

    // Update the window list - check for new/destroyed windows.
    // TODO: don't enumerate all windows every time, use window hooks to monitor for changes
    AddAllWindows();

    // Update all watched windows.
    EnterCriticalSection(&g_csWatchedWindows);

    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        BOOL skip;

        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA *)entry->ListEntry.Flink;

        UpdateWindowData(entry, &skip);
        if (skip)
        {
            entry = nextEntry;
            continue;
        }

        RECT windowRect = { entry->X, entry->Y, entry->X + entry->Width, entry->Y + entry->Width };
        RECT changedArea; // intersection of damage rect with window rect

        // skip windows that aren't in the changed area
        for (UINT i = 0; i < frame->dirty_rects_count; i++)
        {
            if (IntersectRect(&changedArea, &frame->dirty_rects[i], &windowRect))
            {
                status = SendWindowDamageEvent(entry->WindowHandle,
                    changedArea.left - entry->X, // TODO: verify
                    changedArea.top - entry->Y,
                    changedArea.right - entry->X,
                    changedArea.bottom - entry->Y);

                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "SendWindowDamageEvent");
                    goto cleanup;
                }
            }
        }
        entry = nextEntry;
    }

cleanup:
    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

ULONG StartFrameProcessing(IN HANDLE newFrameEvent, IN HANDLE captureErrorEvent, OUT CAPTURE_CONTEXT** capture)
{
    LogVerbose("start");
    AttachToInputDesktop();
    // Initialize capture interfaces, this also initializes framebuffer PFNs
    *capture = CaptureInitialize(newFrameEvent, captureErrorEvent);
    if (!*capture)
        return win_perror("CaptureInitialize");

    ULONG status;
    // send whole screen window, needed even in seamless mode
    status = SendWindowCreate(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowCreate(NULL)");

    // send the whole screen framebuffer map
    status = SendScreenGrants(FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight), (*capture)->grant_refs);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendScreenGrants");

    // this (re)initializes watched windows list
    status = SetSeamlessMode(g_SeamlessMode, TRUE);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SetSeamlessMode");

    status = CaptureStart(*capture);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "CaptureStart");

    LogVerbose("end");
    return ERROR_SUCCESS;
}

void StopFrameProcessing(IN OUT CAPTURE_CONTEXT** capture)
{
    if (!capture)
        return;

    SendWindowDestroy(NULL);

    CaptureTeardown(*capture);
    *capture = NULL;
}

// main event loop
// TODO: refactor into smaller parts
static ULONG WINAPI WatchForEvents  (void)
{
    ULONG eventCount;
    DWORD signaledEvent;
    BOOL vchanIoInProgress;
    ULONG status;
    BOOL exitLoop;
    HANDLE watchedEvents[MAXIMUM_WAIT_OBJECTS];
#if defined(_DEBUG) || defined(DEBUG)
    DWORD dumpLastTime = GetTickCount();
    //UINT64 damageCount = 0, damageCountOld = 0;
#endif

    LogDebug("start");
    HANDLE newFrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    HANDLE captureErrorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    if (!VchanInit(6000))
    {
        LogError("VchanInit() failed");
        return GetLastError();
    }

    HANDLE fullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!fullScreenOnEvent)
        return GetLastError();
    HANDLE fullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!fullScreenOffEvent)
        return GetLastError();
    g_ResolutionChangeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_ResolutionChangeEvent)
        return GetLastError();

    g_VchanClientConnected = FALSE;
    vchanIoInProgress = FALSE;
    exitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize(g_Vchan));
    watchedEvents[0] = g_ShutdownEvent;
    watchedEvents[1] = newFrameEvent;
    watchedEvents[2] = fullScreenOnEvent;
    watchedEvents[3] = fullScreenOffEvent;
    watchedEvents[4] = g_ResolutionChangeEvent;
    watchedEvents[5] = libvchan_fd_for_select(g_Vchan);
    watchedEvents[6] = CreateEvent(NULL, FALSE, FALSE, NULL); // force update event
    watchedEvents[7] = captureErrorEvent;
    eventCount = 8;

    CAPTURE_CONTEXT* capture = NULL;

    while (TRUE)
    {
        status = ERROR_SUCCESS;

        vchanIoInProgress = TRUE;

        // Wait for events.
        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            status = win_perror("WaitForMultipleObjects");
            break;
        }

#if defined(_DEBUG) || defined(DEBUG)
        // dump watched windows every second
        if (GetTickCount() - dumpLastTime > 1000)
        {
            DumpWindows();
            dumpLastTime = GetTickCount();

            // XXX dump performance counters
            //LogDebug("last second damages: %llu", damageCount - damageCountOld);
            //damageCountOld = damageCount;
        }
#endif

        if (0 == signaledEvent)
        {
            // shutdown event
            LogDebug("Shutdown event signaled");
            exitLoop = TRUE;
            break;
        }

        switch (signaledEvent)
        {
        case 1: // new frame available
            LogVerbose("new frame");
            if (g_VchanClientConnected)
            {
                assert(capture);
                ProcessUpdatedWindows(&capture->frame);
            }

            if (capture)
                SetEvent(capture->ready_event); // frame processed
            break;

        case 2: // seamless off event
            LogVerbose("seamless off");
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SetSeamlessMode(FALSE)");
                exitLoop = TRUE;
            }
            break;

        case 3: // seamless on event
            LogVerbose("seamless on");
            status = SetSeamlessMode(TRUE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SetSeamlessMode(TRUE)");
                exitLoop = TRUE;
            }
            break;

        case 4: // resolution change event, signaled by ResolutionChangeThread
            LogVerbose("resolution change");
            // don't explicitly reinitialize capture here
            // if it gets invalidated it'll signal us

            // Params are in g_ResolutionChangeParams in resolution.c
            status = ChangeResolution();
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "ChangeResolution");
                // XXX don't totally fail on resolution change, this will cause qga to
                // be constantly respawned by watchdog and not doing anything constructive
                // TODO: handle MS basic display driver's fixed list of supported resolutions
                //exitLoop = TRUE;
                //break;
            }

            // is it possible to have VM resolution bigger than host set by user?
            if ((g_ScreenWidth < g_HostScreenWidth) && (g_ScreenHeight < g_HostScreenHeight))
                g_SeamlessMode = FALSE; // can't have reliable/intuitive seamless mode in this case

            break;

        case 5: // vchan receive
            if (!g_VchanClientConnected)
            {
                vchanIoInProgress = FALSE;

                LogInfo("A vchan client has connected");

                // needs to be set before enumerating windows so maps get sent
                // (and before sending anything really)
                g_VchanClientConnected = TRUE;

                if (ERROR_SUCCESS != SendProtocolVersion())
                {
                    LogError("SendProtocolVersion failed");
                    exitLoop = TRUE;
                    break;
                }

                // This will probably change the current video mode.
                if (ERROR_SUCCESS != HandleXconf())
                {
                    LogError("HandleXconf failed");
                    exitLoop = TRUE;
                    break;
                }

                status = StartFrameProcessing(newFrameEvent, captureErrorEvent, &capture);
                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "StartCapture");
                    exitLoop = TRUE;
                    break;
                }

                break;
            }

            EnterCriticalSection(&g_VchanCriticalSection);
            LogVerbose("vchan receive, %d bytes", VchanGetReadBufferSize(g_Vchan));

            vchanIoInProgress = FALSE;

            if (!libvchan_is_open(g_Vchan))
            {
                LogError("vchan disconnected");
                exitLoop = TRUE;
                LeaveCriticalSection(&g_VchanCriticalSection);
                break;
            }

            while (VchanGetReadBufferSize(g_Vchan) > 0)
            {
                status = HandleServerData();
                if (ERROR_SUCCESS != status)
                {
                    exitLoop = TRUE;
                    LogError("HandleServerData failed: 0x%x", status);
                    break;
                }
            }
            LeaveCriticalSection(&g_VchanCriticalSection);
            break;

        case 6: // capture error, can be due to a desktop switch or resolution change
            LogDebug("capture error, reinitializing");

            StopFrameProcessing(&capture);
            capture = NULL;

            status = StartFrameProcessing(newFrameEvent, captureErrorEvent, &capture);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "StartCapture");
                exitLoop = TRUE;
                break;
            }
            break;
        }

        if (exitLoop)
            break;
    }

    LogDebug("main loop finished");

    if (g_VchanClientConnected)
        libvchan_close(g_Vchan);

    if (capture)
        StopFrameProcessing(&capture);
    LogInfo("exiting");
    // all handles will be closed on exit anyway

    return exitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static DWORD GetDomainName(OUT char *nameBuffer, IN DWORD nameLength)
{
    DWORD status = ERROR_SUCCESS;
    qdb_handle_t qdb = NULL;
    char *domainName = NULL;

    qdb = qdb_open(NULL);
    if (!qdb)
        return win_perror("qdb_open");

    domainName = qdb_read(qdb, "/name", NULL);
    if (!domainName)
    {
        LogError("Failed to read domain name");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("%S", domainName);
    status = StringCchCopyA(nameBuffer, nameLength, domainName);
    if (FAILED(status))
        win_perror2(status, "StringCchCopyA");

cleanup:
    qdb_free(domainName);
    if (qdb)
        qdb_close(qdb);

    return status;
}

static DWORD GetGuiDomainId(OUT USHORT* gid)
{
    DWORD status = ERROR_SUCCESS;
    qdb_handle_t qdb = NULL;
    char *string_id = NULL;
    int id = 0;

    qdb = qdb_open(NULL);
    if (!qdb)
        return win_perror("qdb_open");

    string_id = qdb_read(qdb, "/qubes-gui-domain-xid", NULL);
    if (!string_id)
    {
        LogError("Failed to read GUI domain id");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("GUI domain id: %S", string_id);

    id = atoi(string_id);
    if (errno == ERANGE || id < 0 || id > USHRT_MAX)
    {
        LogError("GUI domain id is invalid (%S)", string_id);
        status = ERROR_INVALID_DATA;
        goto cleanup;
    }

    status = ERROR_SUCCESS;
    *gid = (USHORT)id;

cleanup:
    qdb_free(string_id);
    if (qdb)
        qdb_close(qdb);

    return status;
}

static ULONG Init(void)
{
    ULONG status;
    WSADATA wsaData;
    WCHAR moduleName[CFG_MODULE_MAX];

    LogDebug("start");

    // This needs to be done first as a safeguard to not start multiple instances of this process.
    g_ShutdownEvent = CreateNamedEvent(QGA_SHUTDOWN_EVENT_NAME);
    if (!g_ShutdownEvent)
    {
        return GetLastError();
    }

    status = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));
    // XXX dirty bits
    /* disabled for now because driver lacks interface for it
    status = CfgReadDword(moduleName, REG_CONFIG_DIRTY_VALUE, &g_UseDirtyBits, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, disabling that feature", REG_CONFIG_DIRTY_VALUE);
        g_UseDirtyBits = FALSE;
    }
    */
    status = CfgReadDword(moduleName, REG_CONFIG_CURSOR_VALUE, &g_DisableCursor, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_CURSOR_VALUE);
        g_DisableCursor = TRUE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_VALUE, &g_SeamlessMode, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (FALSE)", REG_CONFIG_SEAMLESS_VALUE);
        g_SeamlessMode = FALSE;
    }

    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    // XXX needed?
    status = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != status)
    {
        win_perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    status = GetDomainName(g_DomainName, RTL_NUMBER_OF(g_DomainName));
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read domain name, using host name");

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

    status = GetGuiDomainId(&g_GuiDomainId);
    if (status != ERROR_SUCCESS)
        return status;

    LogInfo("Fullscreen desktop name: %S", g_DomainName);

    InitializeListHead(&g_WatchedWindowsList);
    InitializeCriticalSection(&g_csWatchedWindows);
    return ERROR_SUCCESS;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    if (ERROR_SUCCESS != Init())
        return win_perror("Init");

    InitializeCriticalSection(&g_VchanCriticalSection);

    // Call the thread proc directly.
    if (ERROR_SUCCESS != WatchForEvents())
        return win_perror("WatchForEvents");

    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("exiting");
    return ERROR_SUCCESS;
}
