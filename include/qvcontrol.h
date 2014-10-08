#pragma once

#include <windows.h>
#include <tchar.h>
#include "common.h"

extern QV_DIRTY_PAGES *g_pDirtyPages;

ULONG OpenScreenSection();

ULONG CloseScreenSection();

ULONG GetWindowData(
    HWND hWnd,
    QV_GET_SURFACE_DATA_RESPONSE *pQvGetSurfaceDataResponse,
    PFN_ARRAY *pPfnArray
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
    DISPLAY_DEVICE *pQubesDisplayDevice
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
