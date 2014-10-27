#pragma once

#include <windows.h>
#include "common.h"

extern QV_DIRTY_PAGES *g_DirtyPages;

ULONG OpenScreenSection(void);

ULONG CloseScreenSection(void);

ULONG FindQubesDisplayDevice(
    OUT DISPLAY_DEVICE *qubesDisplayDevice
    );

// tells qvideo that given resolution will be set by the system
ULONG SupportVideoMode(
    IN const WCHAR *qubesDisplayDeviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG GetWindowData(
    IN HWND window,
    OUT QV_GET_SURFACE_DATA_RESPONSE *surfaceData,
    OUT PFN_ARRAY *pfnArray
    );

ULONG ChangeVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG RegisterWatchedDC(
    IN HDC dc,
    IN HANDLE damageEvent
    );

ULONG UnregisterWatchedDC(
    IN HDC dc
    );

ULONG SynchronizeDirtyBits(
    IN HDC dc
    );
