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

#include <vchan-common.h>

extern CRITICAL_SECTION g_VchanCriticalSection;
extern struct libvchan *g_Vchan;

BOOL VchanInitServer(IN int port);
BOOL VchanSendMessage(IN const void *header, IN int headerSize, IN const void *data, IN int datasize, IN const WCHAR *what);

#define VCHAN_SEND_MSG(header, body, what) (\
    header.untrusted_len = sizeof(header), \
    VchanSendMessage(&(header), sizeof(header), &(body), sizeof(body), what) \
    )

#define VCHAN_SEND(x, what) VchanSendBuffer(g_Vchan, &(x), sizeof(x), what)
