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
