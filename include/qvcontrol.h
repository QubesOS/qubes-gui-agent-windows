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
	//ULONG uModifications;

	RECT	rcWindow;
	LIST_ENTRY	le;

	BOOL	bVisible;
	BOOL	bOverrideRedirect;
    HWND    ModalParent; // if nonzero, this window is modal in relation to window pointed by this field

	BOOL	bStyleChecked;
	ULONG	uTimeAdded;

	LONG	MaxWidth;
	LONG	MaxHeight;
	PUCHAR	pCompositionBuffer;
	ULONG	uCompositionBufferSize;
	PFN_ARRAY	PfnArray;

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

ULONG GetPfnList(
	PVOID pVirtualAddress,
	ULONG uRegionSize,
	PPFN_ARRAY pPfnArray
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
