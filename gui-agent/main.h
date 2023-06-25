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
extern LONG g_ScreenHeight;
extern LONG g_ScreenWidth;
extern LONG g_HostScreenWidth;
extern LONG g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern HWND g_DesktopWindow;
extern char g_DomainName[256];
extern CRITICAL_SECTION g_csWatchedWindows;

typedef struct _WINDOW_DATA
{
    HWND WindowHandle;
    BOOL IsIconic;
    WCHAR Caption[256];
    WCHAR Class[256];
    int X;
    int Y;
    int Width;
    int Height;

    LIST_ENTRY ListEntry;

    BOOL IsOverrideRedirect;
    HWND ModalParent; // if nonzero, this window is modal in relation to window pointed by this field
} WINDOW_DATA;

typedef struct _BANNED_WINDOWS
{
    HWND Explorer;
    HWND Desktop;
    HWND Taskbar;
    HWND Start;
} BANNED_WINDOWS;

// used when searching for modal window that's blocking another window
typedef struct _MODAL_SEARCH_PARAMS
{
    HWND ParentWindow; // window that's disabled by a modal window, input
    HWND ModalWindow; // modal window that's active, output
} MODAL_SEARCH_PARAMS;

BOOL ShouldAcceptWindow(
    IN HWND window,
    IN const WINDOWINFO *pwi OPTIONAL
    );

WINDOW_DATA *FindWindowByHandle(
    HWND window
    );

ULONG AddWindowWithInfo(
    IN HWND window,
    IN const WINDOWINFO *windowInfo,
    OUT WINDOW_DATA **windowEntry OPTIONAL
    );

ULONG RemoveWindow(IN OUT WINDOW_DATA *entry);

// This (re)initializes watched windows, hooks etc.
ULONG SetSeamlessMode(
    IN BOOL seamlessMode,
    IN BOOL forceUpdate
    );
