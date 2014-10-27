#pragma once
#include <windows.h>

#define RESOLUTION_CHANGE_TIMEOUT 500

typedef struct _RESOLUTION_CHANGE_PARAMS
{
    LONG Width;
    LONG Height;
    LONG Bpp;
    LONG X; // this is needed to send ACK to daemon although it's useless for fullscreen
    LONG Y;
} RESOLUTION_CHANGE_PARAMS;

extern HANDLE g_ResolutionChangeEvent;

void RequestResolutionChange(IN LONG width, IN LONG height, IN LONG bpp, IN LONG x, IN LONG y);

ULONG SetVideoMode(IN ULONG width, IN ULONG height, IN ULONG bpp);

// Reinitialize everything, change resolution (params in g_ResolutionChangeParams).
ULONG ChangeResolution(IN OUT HDC *screenDC, IN HANDLE damageEvent);
