#pragma once
#include <windows.h>

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

extern BOOL g_bUseDirtyBits;
extern BOOL g_bFullScreenMode;
extern LONG g_ScreenHeight;
extern LONG g_ScreenWidth;
extern LONG g_HostScreenWidth;
extern LONG g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern char g_HostName[256];

ULONG HideCursors(void);
ULONG DisableEffects(void);
