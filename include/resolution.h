#pragma once
#include <windows.h>

#define RESOLUTION_CHANGE_TIMEOUT 500

typedef struct
{
    LONG width;
    LONG height;
    LONG bpp;
    LONG x; // this is needed to send ACK to daemon although it's useless for fullscreen
    LONG y;
} RESOLUTION_CHANGE_PARAMS;

extern HANDLE g_ResolutionChangeEvent;

void RequestResolutionChange(LONG width, LONG height, LONG bpp, LONG x, LONG y);
ULONG ChangeResolution(HDC *screenDC, HANDLE damageEvent);
ULONG SetVideoMode(ULONG width, ULONG height, ULONG bpp);
