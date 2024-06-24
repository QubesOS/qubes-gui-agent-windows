/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once
#include <stdint.h>

#include "main.h"

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

ULONG SendScreenGrants(IN size_t numGrants, IN const ULONG* refs);
ULONG SendWindowCreate(IN const WINDOW_DATA *windowData);
ULONG SendWindowDestroy(IN HWND window);
ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset);
ULONG SendWindowHints(IN HWND window, IN uint32_t flags);
ULONG SendScreenHints(void);
ULONG SendWindowUnmap(IN HWND window);
ULONG SendWindowMap(IN const WINDOW_DATA *windowData OPTIONAL); // if windowData == 0, use the whole screen
ULONG SendWindowConfigure(HANDLE window, int x, int y, int width, int height, BOOL popup);
ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height);
ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL);
ULONG SendProtocolVersion(void);
