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

#include <windows.h>
#include <assert.h>

#include "common.h"
#include "main.h"
#include "resolution.h"
#include "send.h"
#include "util.h"

#include <log.h>

// parameters for the resolution change thread
typedef struct _RESOLUTION_THREAD_PARAMS
{
    HANDLE Event; // event to wait on
    LONG Width; // requested resolution
    LONG Height;
} RESOLUTION_THREAD_PARAMS;

struct SUPPORTED_MODES
{
    DWORD Count;
    POINT* Dimensions;
} g_SupportedModes;

void InitVideoModes()
{
    DEVMODEW mode;

    for (g_SupportedModes.Count = 0; ; g_SupportedModes.Count++)
    {
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(NULL, g_SupportedModes.Count, &mode))
            break;
        LogDebug("mode %u: %ux%u %u bpp @ %u, flags 0x%x", g_SupportedModes.Count,
            mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel, mode.dmDisplayFrequency, mode.dmDisplayFlags);
    }

    g_SupportedModes.Dimensions = (POINT*)malloc(g_SupportedModes.Count * sizeof(POINT));

    for (DWORD i = 0; i < g_SupportedModes.Count; i++)
    {
        mode.dmSize = sizeof(mode);
        assert(EnumDisplaySettingsW(NULL, i, &mode));

        g_SupportedModes.Dimensions[i].x = mode.dmPelsWidth;
        g_SupportedModes.Dimensions[i].y = mode.dmPelsHeight;
    }

    LogDebug("Initialized %u supported modes", g_SupportedModes.Count);
}

static ULONG SetVideoModeInternal(IN ULONG width, IN ULONG height)
{
    if (!IS_RESOLUTION_VALID(width, height))
    {
        LogError("Resolution is invalid: %lu x %lu", width, height);
        return ERROR_INVALID_PARAMETER;
    }

    LogInfo("New resolution: %lu x %lu", width, height);
    // ChangeDisplaySettings fails if thread's desktop != input desktop...
    // This can happen on "quick user switch".
    AttachToInputDesktop();

    DEVMODE devMode;
    ZeroMemory(&devMode, sizeof(DEVMODE));
    devMode.dmSize = sizeof(DEVMODE);
    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

    ULONG status = ChangeDisplaySettings(&devMode, 0);
    if (DISP_CHANGE_SUCCESSFUL != status)
    {
        LogError("ChangeDisplaySettings failed: 0x%x", status);
    }

    SetLastError(status);
    return status;
}

// return best-matching supported mode index for desired resolution
DWORD SelectSupportedMode(IN DWORD width, IN DWORD height)
{
    DWORD mode = 0;
    float sim = 0;

    for (DWORD i = 0; i < g_SupportedModes.Count; i++)
    {
        DWORD w = g_SupportedModes.Dimensions[i].x;
        DWORD h = g_SupportedModes.Dimensions[i].y;

        // TODO: filter these when constructing supported mode list
        if (w > g_HostScreenWidth || h > g_HostScreenHeight)
            continue;

        if (w == width && h == height)
        {
            LogDebug("Returning mode %u (%ux%u) for %ux%u", i, w, h, width, height);
            return i;
        }

        float area_cur = w * (float)h;
        float area_req = width * (float)height;
        float inter = min(w, width) * (float)min(h, height);
        float similarity = inter / (float)(area_cur + area_req - inter);

        if (similarity > sim)
        {
            sim = similarity;
            mode = i;
        }
    }

    LogDebug("Returning mode %u (%ux%u) for %ux%u", mode,
        g_SupportedModes.Dimensions[mode].x, g_SupportedModes.Dimensions[mode].y, width, height);
    return mode;
}

ULONG SetVideoMode(IN ULONG width, IN ULONG height)
{
    LogVerbose("%lu x %lu", width, height);

    DWORD mode = SelectSupportedMode(width, height);

    width = g_SupportedModes.Dimensions[mode].x;
    height = g_SupportedModes.Dimensions[mode].y;

    if (width == g_ScreenWidth && height == g_ScreenHeight)
    {
        LogDebug("No change");
        return ERROR_SUCCESS;
    }

    ULONG status = SetVideoModeInternal(width, height);
    if (ERROR_SUCCESS != status)
    {
        g_SeamlessMode = FALSE;

        LogDebug("SetVideoModeInternal failed: 0x%x, keeping original resolution %lux%lu", status, g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        g_ScreenWidth = width;
        g_ScreenHeight = height;
    }

    return status;
}

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
static DWORD WINAPI ResolutionChangeThread(void *param)
{
    DWORD waitResult;
    RESOLUTION_THREAD_PARAMS* args = (RESOLUTION_THREAD_PARAMS*)param;

    while (TRUE)
    {
        // Wait indefinitely for an initial "change resolution" event.
        WaitForSingleObject(args->Event, INFINITE);
        LogDebug("resolution change requested: %dx%d", args->Width, args->Height);

        do
        {
            // If event is signaled again before timeout expires: ignore and wait for another one.
            waitResult = WaitForSingleObject(args->Event, RESOLUTION_CHANGE_TIMEOUT);
            LogVerbose("second wait result: %lu", waitResult);
        } while (waitResult == WAIT_OBJECT_0);

        // If we're here, that means the wait finally timed out.
        // We can change the resolution now.
        LogInfo("resolution change: %dx%d", args->Width, args->Height);

        SetVideoMode(args->Width, args->Height);
    }
    return ERROR_SUCCESS;
}

DWORD RequestResolutionChange(IN LONG width, IN LONG height)
{
    static RESOLUTION_THREAD_PARAMS threadArgs = { 0 };

    // This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
    // after last screen resize message received from gui daemon.
    // This is to not change resolution on every such message (multiple times per second).
    static HANDLE thread = NULL;

    LogVerbose("%dx%d", width, height);

    if (!threadArgs.Event)
        threadArgs.Event = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!threadArgs.Event)
        return win_perror("creating resolution change request event");

    if (!thread)
        thread = CreateThread(NULL, 0, ResolutionChangeThread, &threadArgs, 0, 0);

    if (!thread)
        return win_perror("creating resolution change thread");

    threadArgs.Width = width;
    threadArgs.Height = height;
    if (!SetEvent(threadArgs.Event))
        return win_perror("signaling resolution change request event");

    return ERROR_SUCCESS;
}
