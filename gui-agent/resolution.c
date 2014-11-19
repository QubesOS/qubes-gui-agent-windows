#include <windows.h>

#include "resolution.h"
#include "main.h"
#include "qvcontrol.h"
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
            return perror("SetEvent");
    }
    return ERROR_SUCCESS;
}

// Signals g_ResolutionChangeRequestedEvent
void RequestResolutionChange(IN LONG width, IN LONG height, IN LONG bpp, IN LONG x, IN LONG y)
{
    // Create trigger event for the throttling thread if not yet created.
    if (!g_ResolutionChangeRequestedEvent)
        g_ResolutionChangeRequestedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Create the throttling thread if not yet running.
    if (!g_ResolutionChangeThread)
        g_ResolutionChangeThread = CreateThread(NULL, 0, ResolutionChangeThread, NULL, 0, 0);

    g_ResolutionChangeParams.Width = width;
    g_ResolutionChangeParams.Height = height;
    g_ResolutionChangeParams.Bpp = bpp;
    g_ResolutionChangeParams.X = x;
    g_ResolutionChangeParams.Y = y;
    if (!SetEvent(g_ResolutionChangeRequestedEvent))
        perror("SetEvent");
}

// Actually set video mode through qvideo calls.
static ULONG SetVideoModeInternal(IN ULONG width, IN ULONG height, IN ULONG bpp)
{
    WCHAR *deviceName = NULL;
    DISPLAY_DEVICE device;

    if (!IS_RESOLUTION_VALID(width, height))
    {
        LogError("Resolution is invalid: %lux%lu", width, height);
        return ERROR_INVALID_PARAMETER;
    }

    LogInfo("New resolution: %lux%lu@%lu", width, height, bpp);
    // ChangeDisplaySettings fails if thread's desktop != input desktop...
    // This can happen on "quick user switch".
    AttachToInputDesktop();

    if (ERROR_SUCCESS != FindQubesDisplayDevice(&device))
        return perror("FindQubesDisplayDevice");

    deviceName = (WCHAR *) &device.DeviceName[0];

    LogDebug("DeviceName: %s", deviceName);

    if (ERROR_SUCCESS != SupportVideoMode(deviceName, width, height, bpp))
        return perror("SupportVideoMode");

    if (ERROR_SUCCESS != ChangeVideoMode(deviceName, width, height, bpp))
        return perror("ChangeVideoMode");

    return ERROR_SUCCESS;
}

// set video mode, open screen section
ULONG SetVideoMode(IN ULONG width, IN ULONG height, IN ULONG bpp)
{
    ULONG status = SetVideoModeInternal(width, height, bpp);

    if (ERROR_SUCCESS != status)
    {
        g_SeamlessMode = FALSE;

        LogDebug("SetVideoMode() failed: %lu, keeping original resolution %lux%lu", status, g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        g_ScreenWidth = width;
        g_ScreenHeight = height;
    }

    return OpenScreenSection();
}

// Reinitialize everything, change resolution (params in g_ResolutionChangeParams).
ULONG ChangeResolution(IN OUT HDC *screenDC, IN HANDLE damageEvent)
{
    ULONG status;

    LogDebug("deinitializing");

    if (ERROR_SUCCESS != (status = UnregisterWatchedDC(*screenDC)))
        return status;
    if (ERROR_SUCCESS != (status = CloseScreenSection()))
        return status;
    if (!ReleaseDC(NULL, *screenDC))
        return GetLastError();

    LogDebug("reinitializing");
    status = SetVideoMode(g_ResolutionChangeParams.Width, g_ResolutionChangeParams.Height, g_ResolutionChangeParams.Bpp);

    if (ERROR_SUCCESS != status)
        return status;

    *screenDC = GetDC(NULL);
    if (!(*screenDC))
        return GetLastError();
    if (ERROR_SUCCESS != (status = RegisterWatchedDC(*screenDC, damageEvent)))
        return status;

    status = SendWindowMfns(NULL); // update screen framebuffer
    if (ERROR_SUCCESS != status)
        return status;

    // is it possible to have VM resolution bigger than host set by user?
    if ((g_ScreenWidth < g_HostScreenWidth) && (g_ScreenHeight < g_HostScreenHeight))
        g_SeamlessMode = FALSE; // can't have reliable/intuitive seamless mode in this case

    status = SetSeamlessMode(g_SeamlessMode, TRUE);
    if (ERROR_SUCCESS != status)
        return status;

    LogDebug("done");

    // Reply to the daemon's request (with just the same data).
    // Otherwise daemon won't send screen resize requests again.
    status = SendScreenConfigure(g_ResolutionChangeParams.X, g_ResolutionChangeParams.Y, g_ResolutionChangeParams.Width, g_ResolutionChangeParams.Height);
    if (ERROR_SUCCESS != status)
        return status;

    return ERROR_SUCCESS;
}
