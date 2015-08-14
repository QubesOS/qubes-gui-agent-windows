#pragma once

#include <windows.h>
#include "common.h"

extern QV_DIRTY_PAGES *g_DirtyPages;

ULONG QvFindQubesDisplayDevice(
    OUT DISPLAY_DEVICE *qubesDisplayDevice
    );

// tells qvideo that given resolution will be set by the system
ULONG QvSupportVideoMode(
    IN const WCHAR *qubesDisplayDeviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG QvGetWindowData(
    IN HWND window,
    OUT QV_GET_SURFACE_DATA_RESPONSE *surfaceData
    );

ULONG QvReleaseWindowData(
    IN HWND window
    );

ULONG ChangeVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG QvRegisterWatchedDC(
    IN HDC dc,
    IN HANDLE damageEvent
    );

ULONG QvUnregisterWatchedDC(
    IN HDC dc
    );

ULONG QvSynchronizeDirtyBits(
    IN HDC dc
    );
