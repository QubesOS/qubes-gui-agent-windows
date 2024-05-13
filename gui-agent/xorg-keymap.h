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

extern WORD g_X11ToVk[256];
extern WORD g_KeycodeToScancode[256];
extern const char* g_KeycodeName[256];

/* From X.h */
#define KeyPress            2
#define ButtonPress         4
#define Button1             1
#define Button2             2
#define Button3             3
#define Button4             4
#define Button5             5

#define ShiftMapIndex       0
#define LockMapIndex        1
#define ControlMapIndex     2
#define Mod1MapIndex        3
#define Mod2MapIndex        4
#define Mod3MapIndex        5
#define Mod4MapIndex        6
#define Mod5MapIndex        7
