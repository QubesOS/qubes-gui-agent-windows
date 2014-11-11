#pragma once
#include <stdint.h>

#include "main.h"
#include "vchan.h"

#include "qubes-gui-protocol.h"

// window size hint constants
// http://tronche.com/gui/x/icccm/sec-4.html
#define USPosition  1       // User-specified x, y
#define USSize      2       // User-specified width, height
#define PPosition   4       // Program-specified position
#define PSize       8       // Program-specified size
#define PMinSize    16      // Program-specified minimum size
#define PMaxSize    32      // Program-specified maximum size
#define PResizeInc  64      // Program-specified resize increments
#define PAspect     128 	// Program-specified min and max aspect ratios
#define PBaseSize   256 	// Program-specified base size
#define PWinGravity 512 	// Program-specified window gravity

// window hint flags
#define InputHint          1  // input
#define StateHint          2  // initial_state
#define IconPixmapHint     4  // icon_pixmap
#define IconWindowHint     8  // icon_window
#define IconPositionHint  16  // icon_x & icon_y
#define IconMaskHint      32  // icon_mask
#define WindowGroupHint   64  // window_group
#define MessageHint      128  // (this bit is obsolete)
#define UrgencyHint      256  // urgency

ULONG SendWindowMfns(IN const WINDOW_DATA *watchedDC);
ULONG SendWindowCreate(IN const WINDOW_DATA *watchedDC);
ULONG SendWindowDestroy(IN HWND window);
ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset);
ULONG SendWindowHints(IN HWND window, IN uint32_t flags);
ULONG SendScreenHints(void);
ULONG SendWindowUnmap(IN HWND window);
ULONG SendWindowMap(IN const WINDOW_DATA *watchedDC OPTIONAL); // if watchedDC == 0, use the whole screen
ULONG SendWindowConfigure(IN const WINDOW_DATA *watchedDC OPTIONAL); // if watchedDC == 0, use the whole screen
ULONG SendScreenConfigure(IN UINT32 x, IN UINT32 y, IN UINT32 width, IN UINT32 height);
ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height);
ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL);
ULONG SendProtocolVersion(void);
