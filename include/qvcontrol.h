#pragma once

#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include "common.h"

typedef struct _WATCHED_DC
{
	HWND hWnd;
	HDC hDC;
	ULONG uModifications;
} WATCHED_DC, *PWATCHED_DC;

ULONG GetWindowData(
	HWND hWnd,
	PQV_GET_SURFACE_DATA_RESPONSE pQvGetSurfaceDataResponse
);

ULONG RegisterWatchedDC(
	HDC hDC,
	HANDLE hModificationEvent
);

ULONG UnregisterWatchedDC(
	HDC hDC
);
