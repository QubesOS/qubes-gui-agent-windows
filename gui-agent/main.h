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

#include <list.h>

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

extern BOOL g_UseDirtyBits;
extern BOOL g_SeamlessMode;
extern DWORD g_ScreenHeight;
extern DWORD g_ScreenWidth;
extern DWORD g_HostScreenWidth;
extern DWORD g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern HWND g_DesktopWindow;
extern char g_DomainName[256];
extern USHORT g_GuiDomainId;
extern CRITICAL_SECTION g_csWatchedWindows;

typedef struct _WINDOW_DATA
{
    HWND Handle;
    DWORD Style;
    DWORD ExStyle;
    BOOL IsIconic;
    BOOL IsVisible;
    BOOL DeletePending;
    WCHAR Caption[256];
    WCHAR Class[256];

    // These coords are a minimal bounding rectangle for the visible portion of the window.
    // This may be different than the real RECT of the window.
    int X;
    int Y;
    int Width;
    int Height;

    LIST_ENTRY ListEntry;

    BOOL IsOverrideRedirect;
    HWND ModalParent; // if nonzero, this window is modal in relation to window pointed by this field
} WINDOW_DATA;

BOOL ShouldAcceptWindow(
    IN const WINDOW_DATA* data
    );

WINDOW_DATA *FindWindowByHandle(
    IN HWND window
    );

ULONG AddWindow(
    IN WINDOW_DATA* entry
    );

ULONG RemoveWindow(
    IN OUT WINDOW_DATA *entry
    );

ULONG UpdateWindowData(
    IN OUT WINDOW_DATA* entry
    );

// This (re)initializes watched windows, hooks etc.
ULONG SetSeamlessMode(
    IN BOOL seamlessMode,
    IN BOOL forceUpdate
    );
