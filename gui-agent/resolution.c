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

#include "common.h"
#include "main.h"
#include "resolution.h"
#include "send.h"
#include "util.h"

#include <log.h>

// Signal to actually change resolution to g_ResolutionChangeParams.
// This is pretty ugly but we need to trigger resolution change from a separate thread...
HANDLE g_ResolutionChangeEvent = NULL;

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
// This thread is created in handle_configure().
static HANDLE g_ResolutionChangeThread = NULL;

// Event signaled by handle_configure on receiving screen resize message.
// g_ResolutionChangeThread handles that and throttles too frequent resolution changes.
static HANDLE g_ResolutionChangeRequestedEvent = NULL;

static RESOLUTION_CHANGE_PARAMS g_ResolutionChangeParams = { 0 };

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
// This thread is created in RequestResolutionChange().
static DWORD WINAPI ResolutionChangeThread(void *param)
{
    DWORD waitResult;

    while (TRUE)
    {
        // Wait indefinitely for an initial "change resolution" event.
        WaitForSingleObject(g_ResolutionChangeRequestedEvent, INFINITE);
        LogDebug("resolution change requested: %dx%d", g_ResolutionChangeParams.Width, g_ResolutionChangeParams.Height);

        do
        {
            // If event is signaled again before timeout expires: ignore and wait for another one.
            waitResult = WaitForSingleObject(g_ResolutionChangeRequestedEvent, RESOLUTION_CHANGE_TIMEOUT);
            LogVerbose("second wait result: %lu", waitResult);
        } while (waitResult == WAIT_OBJECT_0);

        // If we're here, that means the wait finally timed out.
        // We can change the resolution now.
        LogInfo("resolution change: %dx%d", g_ResolutionChangeParams.Width, g_ResolutionChangeParams.Height);
        if (!SetEvent(g_ResolutionChangeEvent)) // handled in WatchForEvents, actual resolution change
            return win_perror("SetEvent");
    }
    return ERROR_SUCCESS;
}

// Signals g_ResolutionChangeRequestedEvent
void RequestResolutionChange(IN LONG width, IN LONG height, IN LONG x, IN LONG y)
{
    // Create trigger event for the throttling thread if not yet created.
    if (!g_ResolutionChangeRequestedEvent)
        g_ResolutionChangeRequestedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Create the throttling thread if not yet running.
    if (!g_ResolutionChangeThread)
        g_ResolutionChangeThread = CreateThread(NULL, 0, ResolutionChangeThread, NULL, 0, 0);

    g_ResolutionChangeParams.Width = width;
    g_ResolutionChangeParams.Height = height;
    g_ResolutionChangeParams.X = x;
    g_ResolutionChangeParams.Y = y;
    if (!SetEvent(g_ResolutionChangeRequestedEvent))
        win_perror("SetEvent");

    // ACK the daemon
    // XXX needed for every configure msg received or only the last one in case of throttling?
    SendScreenConfigure(x, y, width, height);
}

// Actually set video mode through qvideo calls.
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

// set video mode
ULONG SetVideoMode(IN ULONG width, IN ULONG height)
{
    // TODO: MS basic display driver has a hardcoded list of supported resolutions, use only those
    LogVerbose("%lu x %lu", width, height);

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

// change screen resolution (params in g_ResolutionChangeParams).
ULONG ChangeResolution()
{
    return SetVideoMode(g_ResolutionChangeParams.Width, g_ResolutionChangeParams.Height);
}
