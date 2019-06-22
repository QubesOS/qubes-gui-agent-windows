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

#include <stddef.h>
#include <stdarg.h>

#ifdef __MINGW32__
#include <driverspecs.h>
#define __in_opt SAL__in_opt
#define __out_bcount_opt SAL__out_bcount_opt
#endif

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)

#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>

#pragma warning(pop)
// C4200: nonstandard extension used :
//        zero-sized array in struct/union
// C4201: nonstandard extension used:
//        nameless struct/union
// C4214: nonstandard extension used:
//        bit field types other than int

#include "debug.h"
#include "common.h"

typedef struct _QV_SURFACE QV_SURFACE, *PQV_SURFACE;
typedef struct _QV_PDEV
{
    HANDLE DisplayHandle;    // Handle to \Device\Screen.
    HDEV EngPdevHandle;      // Engine's handle to PDEV.
    HSURF EngSurfaceHandle;  // Engine's handle to screen surface.
    HPALETTE DefaultPalette; // Handle to the default palette for device.

    ULONG ScreenWidth;       // Visible screen width.
    ULONG ScreenHeight;      // Visible screen height.
    LONG ScreenDelta;        // Distance from one scan line to the next.
    ULONG BitsPerPel;        // Number of bits per pel: only 16, 24, 32 are supported.

    PQV_SURFACE ScreenSurface; // Pointer to screen surface data.
} QV_PDEV, *PQV_PDEV;

#pragma pack(push, 1)
typedef struct _BITMAP_HEADER
{
    BITMAPFILEHEADER FileHeader;
    BITMAPV5HEADER V5Header;
} BITMAP_HEADER;
#pragma pack(pop)

typedef struct _QV_SURFACE
{
    ULONG Width;
    ULONG Height;
    ULONG Stride;
    ULONG BitCount;
    BOOLEAN IsScreen;

    PQV_PDEV Pdev;
    HDRVOBJ DriverObj;
    PEVENT DamageNotificationEvent;

    PVOID PixelData;
    PPFN_ARRAY PfnArray; // kernel address
    PPFN_ARRAY UserPfnArray; // user mapped address

    // page numbers that changed in the surface buffer since the last check
    PQV_DIRTY_PAGES DirtyPages;
} QV_SURFACE, *PQV_SURFACE;

BOOL InitPdev(
    QV_PDEV *,
    DEVMODEW *,
    GDIINFO *,
    DEVINFO *
    );

// Name of the DLL in UNICODE
#define DLL_NAME	L"QubesVideo"

// Four byte tag (characters in reverse order) used for memory allocations
#define ALLOC_TAG	'DDVQ'
