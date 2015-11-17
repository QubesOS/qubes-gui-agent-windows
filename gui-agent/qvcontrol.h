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

#include <windows.h>
#include "common.h"

extern QV_DIRTY_PAGES *g_DirtyPages;

ULONG QvFindQubesDisplayDevice(
    OUT DISPLAY_DEVICE *qubesDisplayDevice
    );

// tells qvideo that given resolution will be set by the system
ULONG QvSupportVideoMode(
    IN const WCHAR *qubesDisplayDeviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG QvGetWindowData(
    IN HWND window,
    OUT QV_GET_SURFACE_DATA_RESPONSE *surfaceData
    );

ULONG QvReleaseWindowData(
    IN HWND window
    );

ULONG ChangeVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    );

ULONG QvRegisterWatchedDC(
    IN HDC dc,
    IN HANDLE damageEvent
    );

ULONG QvUnregisterWatchedDC(
    IN HDC dc
    );

ULONG QvSynchronizeDirtyBits(
    IN HDC dc
    );
