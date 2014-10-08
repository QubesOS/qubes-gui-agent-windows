#pragma once
#include <stdint.h>

#include "main.h"
#include "vchan.h"

#include "qubes-gui-protocol.h"

// window hints constants
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

void send_pixmap_mfns(WATCHED_DC *pWatchedDC);
ULONG send_window_create(WATCHED_DC *pWatchedDC);
ULONG send_window_destroy(HWND hWnd);
ULONG send_window_flags(HWND hWnd, uint32_t flags_set, uint32_t flags_unset);
void send_window_hints(HWND hWnd, uint32_t flags);
void send_screen_hints(void);
ULONG send_window_unmap(HWND hWnd);
ULONG send_window_map(WATCHED_DC *pWatchedDC);
ULONG send_window_configure(WATCHED_DC *pWatchedDC);
ULONG send_screen_configure(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void send_window_damage_event(HWND hWnd, int x, int y, int width, int height);
void send_wmname(HWND hWnd);
void send_protocol_version(void);
