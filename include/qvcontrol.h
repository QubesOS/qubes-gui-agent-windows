#pragma once

#include <windows.h>
#include <tchar.h>
#include "common.h"

extern PQV_DIRTY_PAGES g_pDirtyPages;

ULONG OpenScreenSection();

ULONG CloseScreenSection();

ULONG GetWindowData(
    HWND hWnd,
    PQV_GET_SURFACE_DATA_RESPONSE pQvGetSurfaceDataResponse,
    PPFN_ARRAY pPfnArray
);

ULONG RegisterWatchedDC(
    HDC hDC,
    HANDLE hModificationEvent
);

ULONG UnregisterWatchedDC(
    HDC hDC
);

ULONG SynchronizeDirtyBits(
    HDC hDC
);

ULONG FindQubesDisplayDevice(
    PDISPLAY_DEVICE pQubesDisplayDevice
);

ULONG SupportVideoMode(
    LPTSTR ptszQubesDeviceName,
    ULONG uWidth,
    ULONG uHeight,
    ULONG uBpp
);

ULONG ChangeVideoMode(
    LPTSTR ptszDeviceName,
    ULONG uWidth,
    ULONG uHeight,
    ULONG uBpp
);
