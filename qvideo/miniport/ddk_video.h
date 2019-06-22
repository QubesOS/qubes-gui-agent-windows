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

#include <ntddk.h>
#ifdef __MINGW32__
#include <driverspecs.h>

/* FIXME: gcc doesn't support exceptions */
#define __try if (1)
#define __except(x) if (0)

#endif

//
// Debugging statements. This will remove all the debug information from the
// "free" version.
//

#if DBG
# define VideoDebugPrint(arg) VideoPortDebugPrint arg
#else
# define VideoDebugPrint(arg)
#endif

typedef enum VIDEO_DEBUG_LEVEL
{
    Error = 0,
    Warn,
    Trace,
    Info
} VIDEO_DEBUG_LEVEL;

#define VIDEOPORT_API __declspec(dllimport)

VIDEOPORT_API VOID VideoPortDebugPrint(
    VIDEO_DEBUG_LEVEL DebugPrintLevel,
    __in PSTR DebugMessage,
    ...
    );
