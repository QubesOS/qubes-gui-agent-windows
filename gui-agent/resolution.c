#include <windows.h>

#include "resolution.h"
#include "main.h"
#include "qvcontrol.h"
#include "shell_events.h"
#include "send.h"
#include "util.h"

#include "log.h"

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
// This thread is created in handle_configure().
DWORD WINAPI ResolutionChangeThread(void *param)
{
    DWORD waitResult;

    while (TRUE)
    {
        // Wait indefinitely for an initial "change resolution" event.
        WaitForSingleObject(g_ResolutionChangeRequestedEvent, INFINITE);
        LogDebug("resolution change requested: %dx%d", g_ResolutionChangeParams.width, g_ResolutionChangeParams.height);

        do
        {
            // If event is signaled again before timeout expires: ignore and wait for another one.
            waitResult = WaitForSingleObject(g_ResolutionChangeRequestedEvent, RESOLUTION_CHANGE_TIMEOUT);
            LogVerbose("second wait result: %lu", waitResult);
        } while (waitResult == WAIT_OBJECT_0);

        // If we're here, that means the wait finally timed out.
        // We can change the resolution now.
        LogInfo("resolution change: %dx%d", g_ResolutionChangeParams.width, g_ResolutionChangeParams.height);
        SetEvent(g_ResolutionChangeEvent); // handled in WatchForEvents, actual resolution change
    }
    return 0;
}

// Signals g_ResolutionChangeRequestedEvent
void RequestResolutionChange(LONG width, LONG height, LONG bpp, LONG x, LONG y)
{
    // Create trigger event for the throttling thread if not yet created.
    if (!g_ResolutionChangeRequestedEvent)
        g_ResolutionChangeRequestedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Create the throttling thread if not yet running.
    if (!g_ResolutionChangeThread)
        g_ResolutionChangeThread = CreateThread(NULL, 0, ResolutionChangeThread, NULL, 0, 0);

    g_ResolutionChangeParams.width = width;
    g_ResolutionChangeParams.height = height;
    g_ResolutionChangeParams.bpp = bpp;
    g_ResolutionChangeParams.x = x;
    g_ResolutionChangeParams.y = y;
    SetEvent(g_ResolutionChangeRequestedEvent);
}

// Actually set video mode through qvideo calls.
static ULONG SetVideoModeInternal(ULONG uWidth, ULONG uHeight, ULONG uBpp)
{
    WCHAR *ptszDeviceName = NULL;
    DISPLAY_DEVICE DisplayDevice;

    if (!IS_RESOLUTION_VALID(uWidth, uHeight))
    {
        LogError("Resolution is invalid: %lux%lu", uWidth, uHeight);
        return ERROR_INVALID_PARAMETER;
    }

    LogInfo("New resolution: %lux%lu@%lu", uWidth, uHeight, uBpp);
    // ChangeDisplaySettings fails if thread's desktop != input desktop...
    // This can happen on "quick user switch".
    AttachToInputDesktop();

    if (ERROR_SUCCESS != FindQubesDisplayDevice(&DisplayDevice))
        return perror("FindQubesDisplayDevice");

    ptszDeviceName = (PWCHAR) &DisplayDevice.DeviceName[0];

    LogDebug("DeviceName: %s", ptszDeviceName);

    if (ERROR_SUCCESS != SupportVideoMode(ptszDeviceName, uWidth, uHeight, uBpp))
        return perror("SupportVideoMode");

    if (ERROR_SUCCESS != ChangeVideoMode(ptszDeviceName, uWidth, uHeight, uBpp))
        return perror("ChangeVideoMode");

    return ERROR_SUCCESS;
}

// set video mode, open screen section
ULONG SetVideoMode(ULONG width, ULONG height, ULONG bpp)
{
    ULONG uResult = SetVideoModeInternal(width, height, bpp);

    if (ERROR_SUCCESS != uResult)
    {
        g_SeamlessMode = FALSE;

        LogDebug("SetVideoMode() failed: %lu, keeping original resolution %lux%lu", uResult, g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        g_ScreenWidth = width;
        g_ScreenHeight = height;
    }

    return OpenScreenSection();
}

// Reinitialize everything, change resolution (params in g_ResolutionChangeParams).
ULONG ChangeResolution(HDC *screenDC, HANDLE damageEvent)
{
    ULONG uResult;

    LogDebug("deinitializing");
    if (ERROR_SUCCESS != (uResult = StopShellEventsThread()))
        return uResult;
    if (ERROR_SUCCESS != (uResult = UnregisterWatchedDC(*screenDC)))
        return uResult;
    if (ERROR_SUCCESS != (uResult = CloseScreenSection()))
        return uResult;
    if (!ReleaseDC(NULL, *screenDC))
        return GetLastError();

    LogDebug("reinitializing");
    uResult = SetVideoMode(g_ResolutionChangeParams.width, g_ResolutionChangeParams.height, g_ResolutionChangeParams.bpp);

    if (ERROR_SUCCESS != uResult)
        return uResult;

    *screenDC = GetDC(NULL);
    if (!(*screenDC))
        return GetLastError();
    if (ERROR_SUCCESS != (uResult = RegisterWatchedDC(*screenDC, damageEvent)))
        return uResult;

    send_pixmap_mfns(NULL); // update framebuffer

    // is it possible to have VM resolution bigger than host set by user?
    if ((g_ScreenWidth < g_HostScreenWidth) && (g_ScreenHeight < g_HostScreenHeight))
        g_SeamlessMode = FALSE; // can't have reliable/intuitive seamless mode in this case

    HideCursors();
    DisableEffects();

    if (!g_SeamlessMode)
    {
        send_window_map(NULL); // show desktop window
    }
    else
    {
        if (ERROR_SUCCESS != StartShellEventsThread())
            return uResult;
    }
    LogDebug("done");

    // Reply to the daemon's request (with just the same data).
    // Otherwise daemon won't send screen resize requests again.
    send_screen_configure(g_ResolutionChangeParams.x, g_ResolutionChangeParams.y, g_ResolutionChangeParams.width, g_ResolutionChangeParams.height);

    return ERROR_SUCCESS;
}
