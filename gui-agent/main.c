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

#define DEBUG_DUMP_WINDOWS

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
#include "debug.h"
#include "qubes-io.h"

// windows-utils
#include <log.h>
#include <config.h>
#include <exec.h>
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

// minimal acceptable window dimensions
int g_MinWindowWidth = 0;
int g_MinWindowHeight = 0;

char g_DomainName[256] = "<unknown>";
USHORT g_GuiDomainId = 0;

// TODO: group the list and global window handles in a struct with some accessors
LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

HWND g_DesktopWindow = NULL;
HWND g_TaskbarWindow = NULL;
BOOL g_ShowTaskbar = FALSE;

// these two are always present and "visible" to the winuser APIs regardless of their true state
// may be cloaked by DWM if hidden
// FIXME: search is sometimes "visible" (for winuser) and not DWM cloaked but it's really invisible, detect this state better
BOOL g_StartVisible = FALSE;
HWND g_StartWindow = NULL;
HWND g_SearchWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

#ifdef DEBUG_DUMP_WINDOWS
// FIXME: this fails for UWP apps
void DumpWindowBitmap(const WINDOW_DATA* data)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    WCHAR path[256];
    StringCchPrintf(path, 256, L"c:\\Qubes Tools\\log\\%x_%04d%02d%02d_%02d%02d%02d_%03d.bmp", data->Handle,
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    DWORD status = 0;
    HDC hdc = NULL;
    HBITMAP bm = NULL;
    HDC win_dc = GetWindowDC(data->Handle);
    if (!win_dc)
    {
        status = win_perror("CreateCompatibleDC(window)");
        goto end;
    }

    hdc = CreateCompatibleDC(win_dc);
    if (!hdc)
    {
        status = win_perror("CreateCompatibleDC(window)");
        goto end;
    }

    RECT rect;
    GetWindowRect(data->Handle, &rect);
    int cx = rect.right - rect.left;
    int cy = rect.bottom - rect.top;
    bm = CreateCompatibleBitmap(win_dc, cx, cy);
    if (!bm)
    {
        status = win_perror("CreateCompatibleBitmap(window)");
        goto end;
    }

    SelectObject(hdc, bm);

    if (!BitBlt(hdc, 0, 0, cx, cy, win_dc, 0, 0, SRCCOPY))
    {
        status = win_perror("BitBlt(window)");
        goto end;
    }

    BITMAP b;
    GetObject(bm, sizeof(BITMAP), &b);

    BITMAPFILEHEADER bmfHeader;
    BITMAPINFOHEADER bi;

    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = b.bmWidth;
    bi.biHeight = b.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    DWORD bsize = ((b.bmWidth * bi.biBitCount + 31) / 32) * 4 * b.bmHeight;
    HANDLE dib = GlobalAlloc(GHND, bsize);
    char* ptr = (char*)GlobalLock(dib);
    GetDIBits(win_dc, bm, 0, (UINT)b.bmHeight, ptr, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    HANDLE file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD dibsize = bsize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);
    bmfHeader.bfSize = dibsize;
    bmfHeader.bfType = 0x4D42; // BM
    DWORD wr;
    WriteFile(file, (LPSTR)&bmfHeader, sizeof(BITMAPFILEHEADER), &wr, NULL);
    WriteFile(file, (LPSTR)&bi, sizeof(BITMAPINFOHEADER), &wr, NULL);
    WriteFile(file, (LPSTR)ptr, bsize, &wr, NULL);
    GlobalUnlock(dib);
    GlobalFree(dib);
    CloseHandle(file);

end:
    if (win_dc)
        ReleaseDC(data->Handle, win_dc);
    if (hdc)
        ReleaseDC(data->Handle, hdc);
    if (bm)
        DeleteObject(bm);
    LogVerbose("end (%x)", status);
}

// diagnostic: dump all watched windows
void DumpWindows(void)
{
    WINDOW_DATA *entry;
    static WCHAR* exePath = NULL;

    EnterCriticalSection(&g_csWatchedWindows);
    if (!exePath)
    {
        exePath = malloc(MAX_PATH_LONG_WSIZE);
        if (!exePath)
            goto end;
    }

    if (IsListEmpty(&g_WatchedWindowsList))
        goto end;

    LogDebug("### Window dump:");

    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        exePath[0] = 0;
        DWORD pid;
        GetWindowThreadProcessId(entry->Handle, &pid);
        if (pid)
        {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process != INVALID_HANDLE_VALUE)
            {
                DWORD size = MAX_PATH_LONG;
                QueryFullProcessImageName(process, 0, exePath, &size);
                CloseHandle(process);
            }
        }

        LogLock();
        LogDebugRaw("0x%x: (%6d,%6d) %4dx%4d ",
            entry->Handle, entry->X, entry->Y, entry->Width, entry->Height);
        LogDebugRaw("%c %c ovr=%d [%s] '%s' {%s} parent=0x%x ",
            entry->IsVisible?'V':'-', entry->IsIconic?'_':' ', entry->IsOverrideRedirect,
            entry->Class, entry->Caption, exePath, GetAncestor(entry->Handle, GA_PARENT));
        if (entry->ModalParent)
            LogDebugRaw(" ModalParent=0x%x ", entry->ModalParent);
        LogStyle(entry->Style);
        LogExStyle(entry->ExStyle);
        LogDebugRaw("\n");
        LogUnlock();

        //DumpWindowBitmap(entry);
        entry = (WINDOW_DATA *)entry->ListEntry.Flink;
    }
end:
    LeaveCriticalSection(&g_csWatchedWindows);
}
#endif // DEBUG_DUMP_WINDOWS

// When DWM compositing is enabled (normally always on), most windows are actually smaller
// than their size reported by winuser functions. This is because their edges contain
// invisible grip handles managed by DWM. This function returns actual visible window size.
// This function also accounts for a clipping region possibly defined for the window
// and returns a minimal bounding rectangle that covers the visible region.
ULONG GetRealWindowRect(IN HWND window, OUT RECT* rect)
{
    RECT dwmRect;
    // get real rect of the window as managed by DWM
    HRESULT hresult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &dwmRect, sizeof(RECT));
    if (hresult != S_OK)
        return hresult;

    // monitor info is needed to adjust for DPI scaling
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX monInfo;
    monInfo.cbSize = sizeof(monInfo);
#pragma warning(push)
#pragma warning(disable:4133) // incompatible types - from 'MONITORINFOEX *' to 'LPMONITORINFO' (the function accepts both)
    if (!GetMonitorInfo(monitor, &monInfo))
        return win_perror("GetMonitorInfo failed");
#pragma warning(pop)

    DEVMODE devMode;
    devMode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(monInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode);

    // adjust for DPI scaling
    double scale = (monInfo.rcMonitor.right - monInfo.rcMonitor.left) / (double)devMode.dmPelsWidth;

    rect->left = (LONG)((dwmRect.left - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->right = (LONG)((dwmRect.right - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->top = (LONG)((dwmRect.top - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    rect->bottom = (LONG)((dwmRect.bottom - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    LogDebug("0x%x: rect (%d,%d) %dx%d", window,
        rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);

    // get clipping region
    // TODO: probably need to account for DPI here too
    static HRGN region = NULL;
    if (!region)
        region = CreateRectRgn(0, 0, 0, 0);

    int region_type = GetWindowRgn(window, region);
    if (region_type == SIMPLEREGION || region_type == COMPLEXREGION)
    {
        RECT bounds;
        GetRgnBox(region, &bounds); // bounding box for the region TODO: better clipping for complex ones?
        LogDebug("0x%x: clipping region type %d: bounds (%d,%d) %dx%d", window, region_type,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
        // region coords are relative to the window
        // TODO: should we use DWM's window rect or USER one if they differ?
        bounds.left += rect->left;
        bounds.top += rect->top;
        bounds.right += rect->left;
        bounds.bottom += rect->top;

        RECT clipped;
        IntersectRect(&clipped, rect, &bounds);
        *rect = clipped;
        // clipped is always bounded by rect

        LogDebug("0x%x: clipping region type %d: final rect (%d,%d) %dx%d", window, region_type,
            rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);

        if (window == g_StartWindow)
        {
            // FIXME: make this more reliable
            // start menu still has transparent DWM borders included even after clipping
            // TODO: this will probably break if taskbar is on a different side of the screen
            rect->right -= (bounds.right - bounds.left) - (clipped.right - clipped.left);
            rect->top += (bounds.bottom - bounds.top) - (clipped.bottom - clipped.top);
            LogDebug("0x%x: fixed start rect (%d,%d) %dx%d", window,
                rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);
        }
    }

    return ERROR_SUCCESS;
}

// fills WINDOW_DATA if successful
// if *windowData is NULL, the function allocates a new struct and sets the pointer
// if *windowData is not NULL, the function updates the supplied struct
ULONG GetWindowData(IN HWND window, IN OUT WINDOW_DATA** windowData)
{
    WINDOW_DATA* entry = NULL;
    ULONG status;

    if (windowData == NULL)
        return ERROR_INVALID_PARAMETER;

    RECT rect;
    status = GetRealWindowRect(window, &rect);
    if (!SUCCEEDED(status))
    {
        return win_perror2(status, "GetRealWindowRect");
    }

    if (*windowData != NULL)
    {
        entry = *windowData;
    }
    else
    {
        entry = (WINDOW_DATA*)malloc(sizeof(*entry));
        if (!entry)
        {
            LogError("Failed to malloc entry");
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        *windowData = entry;
    }

    ZeroMemory(entry, sizeof(*entry));

    entry->X = rect.left;
    entry->Y = rect.top;
    entry->Width = rect.right - rect.left;
    entry->Height = rect.bottom - rect.top;
    entry->Handle = window;
    entry->Style = GetWindowLong(window, GWL_STYLE);
    entry->ExStyle = GetWindowLong(window, GWL_EXSTYLE);
    entry->IsIconic = IsIconic(window);
    entry->DeletePending = FALSE;
    GetWindowText(window, entry->Caption, ARRAYSIZE(entry->Caption)); // don't really care about errors here
    GetClassName(window, entry->Class, ARRAYSIZE(entry->Class));

    entry->IsVisible = IsWindowVisible(window);

    DWORD cloaked;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        if (cloaked != 0) // hidden by DWM
            entry->IsVisible = FALSE;
    }

    if (window == g_StartWindow)
        g_StartVisible = entry->IsVisible;

    if (entry->IsVisible)
    {
        // Special treatment for minimized UAC prompts:
        // These tend to show up when pressing ctrl-shift-enter in the "Run" dialog to run as admin.
        // They don't show as normal windows, just flash their icon in the taskbar as if minimized.
        // But, for SOME reason, *sometimes* these windows don't have the iconic flag set, and their size is 0...
        // TODO: enumerate taskbar buttons to properly detect such cases,
        // this seems to require using UI Automation interfaces.
        if (wcscmp(entry->Class, L"$$$Secure UAP Dummy Window Class For Interim Dialog") == 0)
        {
            // For now we hide these windows and mark the taskbar to be shown
            // because programatically activating them doesn't seem to work.
            entry->IsVisible = FALSE;
            g_ShowTaskbar = TRUE;
            LogVerbose("0x%x: UAC prompt", window);
            return ERROR_SUCCESS;
        }

        // check if we're modal to some other window
        HWND owner = GetWindow(window, GW_OWNER);
        if (owner)
        {
            BOOL ownerDisabled = GetWindowLong(owner, GWL_STYLE) & WS_DISABLED;
            if (ownerDisabled)
                entry->ModalParent = owner;
            else
                entry->ModalParent = NULL;
        }

        // FIXME: better prevention of large popup windows that can obscure dom0 screen
        // this is mainly for the logon window (which is screen-sized without caption)
        if (entry->Width == g_ScreenWidth && entry->Height == g_ScreenHeight)
        {
            LogDebug("0x%x: popup too large: %dx%d, screen %dx%d",
                     entry->Handle, entry->Width, entry->Height, g_ScreenWidth, g_ScreenHeight);
            entry->IsOverrideRedirect = FALSE;
        }
        else
        {
            // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
            if ((entry->Style & WS_CAPTION) == WS_CAPTION)
            {
                // normal window
                entry->IsOverrideRedirect = FALSE;
            }
            else if (((entry->Style & WS_SYSMENU) == WS_SYSMENU) && ((entry->ExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
            {
                // Metro apps without WS_CAPTION.
                // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
                // but I guess MS doesn't adhere to its own standards...
                entry->IsOverrideRedirect = FALSE;
            }
            else
            {
                entry->IsOverrideRedirect = TRUE;
            }
        }
    }

    if (entry->IsOverrideRedirect && entry->IsVisible)
    {
        LogVerbose("0x%x: popup %dx%d", entry->Handle, entry->Width, entry->Height);
    }

    return ERROR_SUCCESS;
}

// watched window critical section must be entered
// also sends creation notifications to gui daemon
ULONG AddWindow(IN WINDOW_DATA* entry)
{
    ULONG status = ERROR_SUCCESS;
    LogVerbose("start, handle 0x%x, visible %d, iconic %d", entry->Handle, entry->IsVisible, entry->IsIconic);
    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // send window creation info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowCreate");
            goto end;
        }

        // map (show) the window if it's visible OR minimized
        if (entry->IsIconic || entry->IsVisible)
        {
            status = SendWindowMap(entry);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SendWindowMap");
                goto end;
            }

            if (entry->IsIconic)
            {
                status = SendWindowFlags(entry->Handle, WINDOW_FLAG_MINIMIZE, 0);
                if (ERROR_SUCCESS != status)
                    goto end;
            }
        }

        status = SendWindowName(entry->Handle, entry->Caption);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowName");
            goto end;
        }
    }

end:
    LogVerbose("end (%x)", status);
    return status;
}

// Remove window from the list and free memory.
// Watched windows list critical section must be entered.
ULONG RemoveWindow(IN OUT WINDOW_DATA *entry)
{
    ULONG status = ERROR_INVALID_PARAMETER;

    LogVerbose("start");

    if (!entry)
        goto end;

    LogDebug("0x%x", entry->Handle);

    RemoveEntryList(&entry->ListEntry);

    if (entry->Handle == g_StartWindow)
        g_StartVisible = FALSE;

    if (g_VchanClientConnected)
    {
        status = SendWindowUnmap(entry->Handle);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowUnmap");
            goto end;
        }

        if (entry->Handle) // never destroy screen "window"
        {
            status = SendWindowDestroy(entry->Handle);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SendWindowDestroy");
                goto end;
            }
        }
    }

    free(entry);
    status = ERROR_SUCCESS;
end:
    LogVerbose("end (%x)", status);
    return status;
}

// EnumWindows callback for adding all eligible top-level windows to the list.
// watched windows critical section must be entered
static BOOL CALLBACK AddWindowsProc(IN HWND window, IN LPARAM lParam)
{
    ULONG status;

    WINDOW_DATA* data = FindWindowByHandle(window);
    if (data) // already in the list
    {
        LogVerbose("0x%x: end (existing)", window);
        return TRUE; // skip to next window
    }

    // window not in list, get its data and add
    status = GetWindowData(window, &data);
    if (status != ERROR_SUCCESS)
    {
        LogVerbose("0x%x: end (error)", window);
        return TRUE;
    }

    if (!ShouldAcceptWindow(data))
    {
        LogVerbose("0x%x: end (skipping)", window);
        return TRUE;
    }

    status = AddWindow(data);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "AddWindow");
        LogVerbose("0x%x: end (add failed, exiting)", window);
        return FALSE; // stop enumeration, fatal error occurred (should probably exit process at this point)
    }

    LogVerbose("0x%x: end (new, added)", window);
    return TRUE;
}

// Adds all top-level windows to the watched list.
// watched windows critical section must be entered
static ULONG AddAllWindows(void)
{
    LogVerbose("start");

    g_TaskbarWindow = FindWindow(L"Shell_TrayWnd", 0);
    g_ShowTaskbar = FALSE;

    ULONG status = ERROR_SUCCESS;
    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, 0))
        status = win_perror("EnumWindows");

    if (g_TaskbarWindow)
    {
        WINDOW_DATA* taskbarEntry = FindWindowByHandle(g_TaskbarWindow);
        if (g_ShowTaskbar)
        {
            if (taskbarEntry) // taskbar is already tracked
                goto end;

            LogDebug("showing taskbar");
            ShowWindow(g_TaskbarWindow, SW_SHOW);
            taskbarEntry = NULL;
            status = GetWindowData(g_TaskbarWindow, &taskbarEntry);
            if (status != ERROR_SUCCESS)
            {
                win_perror2(status, "GetWindowData(taskbar)");
                goto end;
            }
            status = AddWindow(taskbarEntry);
        }
        else
        {
            if (!taskbarEntry) // taskbar is not tracked
                goto end;
            status = RemoveWindow(taskbarEntry);
            ShowWindow(g_TaskbarWindow, SW_HIDE);
        }
    }

end:
    LogVerbose("end (%x)", status);
    return status;
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
        LogVerbose("seamless mode, adding all windows");
        // Add all eligible windows to watch list.
        // Since this is a switch from fullscreen, no windows were watched.
        EnterCriticalSection(&g_csWatchedWindows);
        status = AddAllWindows();
        LeaveCriticalSection(&g_csWatchedWindows);

        if (g_TaskbarWindow)
            ShowWindow(g_TaskbarWindow, SW_HIDE);
    }

    LogVerbose("end (%x)", status);
    return status;
}

// set fullscreen/seamless mode
ULONG SetSeamlessMode(IN BOOL seamlessMode, IN BOOL forceUpdate)
{
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start");
    LogDebug("Seamless mode changing to %d", seamlessMode);

    if (g_SeamlessMode == seamlessMode && !forceUpdate)
        goto end; // nothing to do

    status = CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, seamlessMode, NULL);
    if (status != ERROR_SUCCESS)
        LogWarning("Failed to write seamless mode registry value");

    if (!seamlessMode)
    {
        // show the screen window
        status = SendWindowMap(NULL);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowMap(NULL)");
            goto end;
        }
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
        {
            win_perror2(status, "SendWindowUnmap(NULL)");
            goto end;
        }
    }

    // ResetWatch removes all watched windows.
    // If seamless mode is on, top-level windows are added to watch list.
    status = ResetWatch(seamlessMode);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "ResetWatch");
        goto end;
    }

    g_SeamlessMode = seamlessMode;

    LogInfo("Seamless mode changed to %d", seamlessMode);
    status = ERROR_SUCCESS;

end:
    LogVerbose("end (%x)", status);
    return status;
}

WINDOW_DATA *FindWindowByHandle(IN HWND window)
{
    WINDOW_DATA *entry;

    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        if (window == entry->Handle)
            return entry;

        entry = (WINDOW_DATA *)entry->ListEntry.Flink;
    }

    return NULL;
}

// filters unwanted windows (not visible, too small etc)
// assumes window state is up to date
BOOL ShouldAcceptWindow(IN const WINDOW_DATA *data)
{
    if (!data->IsVisible)
        return FALSE;

    if (!g_ShowTaskbar && data->Handle == g_TaskbarWindow)
        return FALSE;

    if (data->DeletePending)
        return FALSE;

    if (data->Handle == GetShellWindow())
        return FALSE;

    // hide search regardless of its state if start is being shown
    // FIXME: this is a workaround for search being detected as visible and not DWM-cloaked
    // even if it's really invisible/transparent
    if (g_StartVisible && data->Handle == g_SearchWindow)
    {
        LogDebug("rejecting Search because Start is visible");
        return FALSE;
    }

    // too small?
    if (data->Width < g_MinWindowWidth || data->Height < g_MinWindowHeight)
        return FALSE;

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (data->Style & WS_CHILD)
    {
        LogVerbose("ignoring child window"); // this shouldn't happen as we only enumerate top-level windows
        return FALSE;
    }

    // this style seems to be used exclusively by helper windows that aren't visible despite having WS_VISIBLE style
    if (data->ExStyle & WS_EX_NOACTIVATE)
    {
        LogVerbose("ignoring WS_EX_NOACTIVATE");
        return FALSE;
    }
    /*
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
    */

    return TRUE;
}

// Refresh data about a window, send notifications to gui daemon if needed.
// Marks the window for removal from the list if the new state makes it no longer eligible.
// Watched windows critical section must be entered.
static ULONG UpdateWindowData(IN OUT WINDOW_DATA *windowData)
{
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start, 0x%x", windowData->Handle);

    WINDOW_DATA data;
    WINDOW_DATA* ptr = &data;

    if (!IsWindow(windowData->Handle))
    {
        LogDebug("0x%x is destroyed, marking for removal", windowData->Handle);
        windowData->DeletePending = TRUE;
        goto end;
    }

    // get current window state
    status = GetWindowData(windowData->Handle, &ptr);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "GetWindowData");
        goto end;
    }

    if (windowData->IsVisible != data.IsVisible)
    {
        windowData->IsVisible = data.IsVisible;
        LogDebug("0x%x IsVisible changed to %d", data.Handle, data.IsVisible);
        if (!data.IsVisible && !data.IsIconic)
            goto end; // skip other stuff, this window will be removed
    }

    if (windowData->Style != data.Style)
    {
        windowData->Style = data.Style;
        LogDebug("0x%x style changed to 0x%x", data.Handle, data.Style);
    }

    if (windowData->ExStyle != data.ExStyle)
    {
        windowData->ExStyle = data.ExStyle;
        LogDebug("0x%x exstyle changed to 0x%x", data.Handle, data.ExStyle);
    }

    // caption
    if (0 != wcscmp(windowData->Caption, data.Caption))
    {
        // caption changed
        StringCchCopy(windowData->Caption, ARRAYSIZE(windowData->Caption), data.Caption);
        status = SendWindowName(windowData->Handle, windowData->Caption);
        if (status != ERROR_SUCCESS)
            goto end;
    }

    // minimized state changed
    if (data.IsIconic)
    {
        if (!windowData->IsIconic)
        {
            LogDebug("0x%x became minimized", windowData->Handle);
            windowData->IsIconic = TRUE;
            status = SendWindowFlags(windowData->Handle, WINDOW_FLAG_MINIMIZE, 0);
        }
        // ignore position changes, iconic windows have coords like (-32000,-32000)
        goto end;
    }
    else
    {
        if (windowData->IsIconic)
        {
            LogVerbose("0x%x became restored", windowData->Handle);
            status = SendWindowFlags(windowData->Handle, 0, WINDOW_FLAG_MINIMIZE); // unset minimize
            if (status != ERROR_SUCCESS)
                goto end;
        }
        windowData->IsIconic = FALSE;
    }

    // coords
    BOOL updateNeeded = (windowData->X != data.X || windowData->Y != data.Y ||
        windowData->Width != data.Width || windowData->Height != data.Height);

    if (updateNeeded)
    {
        LogVerbose("coords changed: 0x%x (%d,%d) %dx%d -> (%d,%d) %dx%d",
            windowData->Handle, windowData->X, windowData->Y, windowData->Width, windowData->Height,
            data.X, data.Y, data.Width, data.Height);

        windowData->X = data.X;
        windowData->Y = data.Y;
        windowData->Width = data.Width;
        windowData->Height = data.Height;

        status = SendWindowConfigure(windowData->Handle,
            windowData->X, windowData->Y, windowData->Width, windowData->Height, windowData->IsOverrideRedirect);

        if (status != ERROR_SUCCESS)
            goto end;
    }

    // TODO: should we care about style changes? some of them affect Z-order (topmost etc)

    if (data.ModalParent != windowData->ModalParent)
    {
        LogDebug("0x%x: modal parent changed from 0x%x to 0x%x",
            data.Handle, windowData->ModalParent, data.ModalParent);
        windowData->ModalParent = data.ModalParent;
        // need to toggle map since this is the only way to change modal status for gui daemon
        status = SendWindowUnmap(data.Handle);
        if (ERROR_SUCCESS != status)
            goto end;

        status = SendWindowMap(windowData);
        if (ERROR_SUCCESS != status)
            goto end;
    }

    status = ERROR_SUCCESS;

end:
    if (!windowData->DeletePending && !ShouldAcceptWindow(windowData))
    {
        LogDebug("0x%x no longer eligible, marking for removal", windowData->Handle);
        windowData->DeletePending = TRUE;
    }

    LogVerbose("end (%x)", status);
    return status;
}

// Called after receiving new frame.
static ULONG ProcessNewFrame(IN const CAPTURE_FRAME* frame)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    HWND oldDesktopWindow = g_DesktopWindow;
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start");
    if (!g_SeamlessMode)
    {
        if (frame->dirty_rects_count == 0)
        {
            // normally we don't get frames with 0 dirty rects unless it's the 1st one
            // then refresh everything
            LogDebug("no dirty rects, updating whole screen");
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

        LogVerbose("end (fullscreen)");
        return ERROR_SUCCESS;
    }

    EnterCriticalSection(&g_csWatchedWindows);

    // TODO: don't enumerate all windows every time, use window hooks to monitor for changes

    // Update state of all tracked windows. If a window is no longer eligible (destroyed, hidden...)
    // then mark it for removal but keep in the list for now, so AddAllWindows() can skip them.
    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;
        status = UpdateWindowData(entry);
        if (status != ERROR_SUCCESS)
        {
            win_perror2(status, "UpdateWindowData");
            entry->DeletePending = TRUE;
            // TODO: exit if there was a vchan failure and we not just failed to get window data
        }

        entry = nextEntry;
    }

    // Enumerate all top-level windows and all eligible ones to the list if not already there.
    AddAllWindows();

    // Remove windows marked for deletion.
    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;

        if (entry->DeletePending)
        {
            status = RemoveWindow(entry);
            if (status != ERROR_SUCCESS)
            {
                win_perror2(status, "RemoveWindow");
                goto cleanup;
            }
        }

        entry = nextEntry;
    }

    // send damage notifications
    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;

        if (entry->IsIconic) // minimized, don't care
            goto skip;

        RECT windowRect = { entry->X, entry->Y, entry->X + entry->Width, entry->Y + entry->Height };
        RECT changedArea; // intersection of damage rect with window rect

        // skip windows that aren't in the changed area
        for (UINT i = 0; i < frame->dirty_rects_count; i++)
        {
            if (IntersectRect(&changedArea, &frame->dirty_rects[i], &windowRect))
            {
                LogVerbose("damage for 0x%x: window (%d,%d) %dx%d, damage (%d,%d) %dx%d, intersect (%d,%d) %dx%d",
                    entry->Handle, entry->X, entry->Y, entry->Width, entry->Height,
                    frame->dirty_rects[i].left, frame->dirty_rects[i].top,
                    frame->dirty_rects[i].right - frame->dirty_rects[i].left, frame->dirty_rects[i].bottom - frame->dirty_rects[i].top,
                    changedArea.left, changedArea.top, changedArea.right - changedArea.left, changedArea.bottom - changedArea.top);

                status = SendWindowDamageEvent(entry->Handle,
                    changedArea.left - entry->X, // window-relative coords
                    changedArea.top - entry->Y,
                    changedArea.right - changedArea.left, // size
                    changedArea.bottom - changedArea.top);

                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "SendWindowDamageEvent");
                    goto cleanup;
                }
            }
        }
skip:
        entry = nextEntry;
    }

cleanup:
    LeaveCriticalSection(&g_csWatchedWindows);
    LogVerbose("end (%x)", status);
    return status;
}

ULONG StartFrameProcessing(IN HANDLE newFrameEvent, IN HANDLE captureErrorEvent, OUT CAPTURE_CONTEXT** capture)
{
    LogVerbose("start");
    AttachToInputDesktop();
    // Initialize capture interfaces, this also initializes framebuffer PFNs
    *capture = CaptureInitialize(newFrameEvent, captureErrorEvent);
    if (!(*capture))
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
    LogVerbose("start");
    if (!capture)
        return;

    SendWindowUnmap(NULL);
    SendWindowDestroy(NULL);

    CaptureTeardown(*capture);
    *capture = NULL;
    LogVerbose("end");
}

const WINDOW_DATA* GetWindowDataCb(IN HWND window)
{
    const WINDOW_DATA* data = NULL;
    EnterCriticalSection(&g_csWatchedWindows);
    data = FindWindowByHandle(window);
    LeaveCriticalSection(&g_csWatchedWindows);
    return data;
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
#ifdef DEBUG_DUMP_WINDOWS
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
    //watchedEvents[6] = CreateEvent(NULL, FALSE, FALSE, NULL); // force update event
    watchedEvents[6] = captureErrorEvent;
    eventCount = 7;

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

#ifdef DEBUG_DUMP_WINDOWS
        if (g_SeamlessMode)
        {
            // dump watched windows every second
            if (GetTickCount() - dumpLastTime > 1000)
            {
                DumpWindows();
                dumpLastTime = GetTickCount();

                // XXX dump performance counters
                //LogDebug("last second damages: %llu", damageCount - damageCountOld);
                //damageCountOld = damageCount;
            }
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
                ProcessNewFrame(&capture->frame);
            }

            if (capture)
                SetEvent(capture->ready_event); // frame processed
            break;

        case 2:
            LogVerbose("fullscreen on");
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SetSeamlessMode(FALSE)");
                exitLoop = TRUE;
            }
            break;

        case 3:
            LogVerbose("fullscreen off");
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
                libvchan_cleanup(g_Vchan); // needed to cleanup xenstore entry

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
                status = HandleServerData(GetWindowDataCb);
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

    EnterCriticalSection(&g_VchanCriticalSection);
    if (g_VchanClientConnected)
    {
        libvchan_close(g_Vchan);
        g_VchanClientConnected = FALSE;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

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

    EnableUIAccess();
    status = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));

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

    g_MinWindowWidth = GetSystemMetrics(SM_CXMIN);
    g_MinWindowHeight = GetSystemMetrics(SM_CYMIN);
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
