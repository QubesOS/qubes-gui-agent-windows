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

#include <windows.h>

#include "vchan.h"

#include <libvchan.h>
#include <vchan-common.h>
#include <log.h>

CRITICAL_SECTION g_VchanCriticalSection;

struct libvchan *g_Vchan = NULL;

BOOL VchanSendMessage(IN const struct msg_hdr *header, IN int headerSize, IN const void *data, IN int dataSize, IN const WCHAR *what)
{
    int status;

    LogVerbose("msg 0x%x (%s) for window 0x%x, size %d", header->type, what, header->window, header->untrusted_len);
    status = VchanSendBuffer(g_Vchan, header, headerSize, what);
    if (status < 0)
        return FALSE;

    status = VchanSendBuffer(g_Vchan, data, dataSize, what);
    if (status < 0)
        return FALSE;

    return TRUE;
}

BOOL VchanInit(IN int domain, IN int port)
{
    // We give a 5 minute timeout here because xeniface can take some time
    // to load the first time after reboot after pvdrivers installation.

    // TODO: vchan buffer size here should be able to contain MFN dump for the desktop,
    // which limits the resolution: X * Y * 4(bpp) / 4096(page size) * 4(mfn size).
    // THIS IS PROBABLY A BUG THOUGH: we should be able to send MSG_MFNDUMP in chunks,
    // but currently vchan send hangs if the data size is higher than the vchan buffer size.
    // Investigate if this is a problem with windows vchan implementation or something else.
    // 64k allows for 4 x 1440p screens.

    g_Vchan = VchanInitServer(domain, port, 65536, 5 * 60 * 1000);
    if (!g_Vchan)
    {
        LogError("VchanInitServer failed");
        return FALSE;
    }

    return TRUE;
}
