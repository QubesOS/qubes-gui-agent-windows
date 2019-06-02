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

#pragma warning(disable: 4201)
#ifdef __MINGW32__
#include <ntdef.h>
#endif
#include <dderror.h>
#include <devioctl.h>
#include <miniport.h>
#include <ntddvdeo.h>
#include <video.h>

#ifdef __MINGW32__
#include <windows.h>
#include <specstrings.h>
#define __inout_bcount SAL__inout_bcount
#endif

// miniport headers don't include list macros for some ungodly reason...
#include "list.h"
#include "memory.h"

#define QFN "[QVMINI] " __FUNCTION__ ": "

// device extension, per-adapter data
typedef struct _QVMINI_DX
{
    PSPIN_LOCK BufferListLock;
    LIST_ENTRY BufferList;
} QVMINI_DX, *PQVMINI_DX;

VP_STATUS __checkReturn HwVidFindAdapter(
    __in void *HwDeviceExtension,
    __in void *HwContext,
    __in WCHAR *ArgumentString,
    __inout_bcount(sizeof(VIDEO_PORT_CONFIG_INFO)) VIDEO_PORT_CONFIG_INFO *ConfigInfo,
    __out UCHAR *Again
    );

BOOLEAN __checkReturn HwVidInitialize(
    __in void *HwDeviceExtension
    );

BOOLEAN __checkReturn HwVidStartIO(
    __in void *HwDeviceExtension,
    __in_bcount(sizeof(VIDEO_REQUEST_PACKET)) VIDEO_REQUEST_PACKET *RequestPacket
    );

ULONG __checkReturn DriverEntry(
    __in void *Context1,
    __in void *Context2
    );
