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
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "common.h"
#include "send.h"
#include "main.h"
#include "qvcontrol.h"
#include "vchan.h"

#include <qubes-gui-protocol.h>
#include <log.h>

#include <strsafe.h>

// Get PFNs of the screen from QVideo driver and prepare relevant shm_cmd struct.
static ULONG PrepareShmCmd(OUT struct shm_cmd **shmCmd)
{
    QV_GET_SURFACE_DATA_RESPONSE surfaceData;
    ULONG status;
    ULONG shmCmdSize = 0;
    ULONG width;
    ULONG height;
    ULONG bpp;
    ULONG i;

    if (!shmCmd)
        return ERROR_INVALID_PARAMETER;

    *shmCmd = NULL;

    LogDebug("fullcreen capture");

    // this will map driver-managed pfn list into the process, user address is in the response struct
    status = QvGetWindowData(NULL, &surfaceData);
    if (ERROR_SUCCESS != status)
        return perror2(status, "QvGetWindowData");

    width = surfaceData.Width;
    height = surfaceData.Height;
    bpp = surfaceData.Bpp;

    LogDebug("screen %dx%d @ %d bpp, number of pfns: %lu", width, height, bpp, surfaceData.PfnArray->NumberOf4kPages);

    shmCmdSize = sizeof(struct shm_cmd) + surfaceData.PfnArray->NumberOf4kPages * sizeof(uint32_t);

    *shmCmd = (struct shm_cmd*) malloc(shmCmdSize);
    if (*shmCmd == NULL)
    {
        LogError("Failed to allocate %d bytes for shm_cmd", shmCmdSize);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    (*shmCmd)->shmid = 0;
    (*shmCmd)->width = width;
    (*shmCmd)->height = height;
    (*shmCmd)->bpp = bpp;
    (*shmCmd)->off = 0;
    (*shmCmd)->num_mfn = surfaceData.PfnArray->NumberOf4kPages;
    (*shmCmd)->domid = 0;

    for (i = 0; i < surfaceData.PfnArray->NumberOf4kPages; i++)
    {
        assert(surfaceData.PfnArray->Pfn[i] == (uint32_t)surfaceData.PfnArray->Pfn[i]);
        (*shmCmd)->mfns[i] = (uint32_t)surfaceData.PfnArray->Pfn[i]; // on x64 pfn numbers are 64bit
    }

    // unmap the pfn array
    status = QvReleaseWindowData(NULL);
    if (ERROR_SUCCESS != status)
        perror2(status, "QvReleaseWindowData");

    return status;
}

ULONG SendScreenMfns(void)
{
    ULONG status;
    struct shm_cmd *shmCmd = NULL;
    struct msg_hdr header;
    UINT32 size;

    LogVerbose("start");

    status = PrepareShmCmd(&shmCmd);
    if (ERROR_SUCCESS != status)
        return perror2(status, "PrepareShmCmd");

    if (shmCmd->num_mfn == 0 || shmCmd->num_mfn > MAX_MFN_COUNT)
    {
        LogError("invalid pfn count: %lu", shmCmd->num_mfn);
        free(shmCmd);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    size = shmCmd->num_mfn * sizeof(uint32_t);

    header.type = MSG_MFNDUMP;
    header.window = 0; // screen
    header.untrusted_len = sizeof(struct shm_cmd) + size;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND(header, L"MSG_MFNDUMP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return perror2(ERROR_UNIDENTIFIED_ERROR, "VCHAN_SEND(header)");
    }

    status = VchanSendBuffer(g_Vchan, shmCmd, sizeof(struct shm_cmd) + size, L"shm_cmd");
    LeaveCriticalSection(&g_VchanCriticalSection);
    if (!status)
        return perror2(status, "VchanSendBuffer");

    free(shmCmd);

    return ERROR_SUCCESS;
}

ULONG SendWindowCreate(IN const WINDOW_DATA *windowData)
{
    WINDOWINFO wi;
    struct msg_hdr header;
    struct msg_create createMsg;
    ULONG status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    wi.cbSize = sizeof(wi);
    /* special case for full screen */
    if (windowData == NULL)
    {
        QV_GET_SURFACE_DATA_RESPONSE surfaceData;

        LogDebug("fullscreen");
        /* TODO: multiple screens? */
        wi.rcWindow.left = 0;
        wi.rcWindow.top = 0;

        status = QvGetWindowData(NULL, &surfaceData);
        if (ERROR_SUCCESS != status)
            return perror2(status, "QvGetWindowData");

        wi.rcWindow.right = surfaceData.Width;
        wi.rcWindow.bottom = surfaceData.Height;

        QvReleaseWindowData(NULL); // unmap the pfn array (TODO: make the mapping optional since it's not used here)

        header.window = 0;
    }
    else
    {
        LogDebug("hwnd=0x%x, (%d,%d) %dx%d, override=%d", windowData->WindowHandle,
                 windowData->X, windowData->Y, windowData->Width, windowData->Height,
                 windowData->IsOverrideRedirect);

#pragma warning(suppress:4311)
        header.window = (uint32_t)windowData->WindowHandle;
        wi.rcWindow.left = windowData->X;
        wi.rcWindow.top = windowData->Y;
        wi.rcWindow.right = windowData->X + windowData->Width;
        wi.rcWindow.bottom = windowData->Y + windowData->Height;
    }

    header.type = MSG_CREATE;

    createMsg.x = wi.rcWindow.left;
    createMsg.y = wi.rcWindow.top;
    createMsg.width = wi.rcWindow.right - wi.rcWindow.left;
    createMsg.height = wi.rcWindow.bottom - wi.rcWindow.top;
#pragma warning(suppress:4311)
    createMsg.parent = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
    createMsg.override_redirect = windowData ? windowData->IsOverrideRedirect : FALSE;
    LogDebug("(%d,%d) %dx%d", createMsg.x, createMsg.y, createMsg.width, createMsg.height);

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND_MSG(header, createMsg, L"MSG_CREATE"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (windowData)
    {
        status = SendWindowHints(windowData->WindowHandle, PPosition); // program-specified position
        if (ERROR_SUCCESS != status)
            return status;
    }

    return ERROR_SUCCESS;
}

ULONG SendWindowDestroy(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogDebug("0x%x", window);
    header.type = MSG_DESTROY;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(header, L"MSG_DESTROY");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset)
{
    struct msg_hdr header;
    struct msg_window_flags flags;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogDebug("0x%x: set 0x%x, unset 0x%x", window, flagsToSet, flagsToUnset);
    header.type = MSG_WINDOW_FLAGS;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    flags.flags_set = flagsToSet;
    flags.flags_unset = flagsToUnset;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, flags, L"MSG_WINDOW_FLAGS");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowHints(IN HWND window, IN uint32_t flags)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    hintsMsg.flags = flags;
    LogDebug("flags: 0x%lx", flags);

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, hintsMsg, L"MSG_WINDOW_HINTS");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendScreenHints(void)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    hintsMsg.flags = PMinSize; // minimum size
    hintsMsg.min_width = MIN_RESOLUTION_WIDTH;
    hintsMsg.min_height = MIN_RESOLUTION_HEIGHT;
    LogDebug("min %dx%d", hintsMsg.min_width, hintsMsg.min_height);

    header.window = 0; // screen
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, hintsMsg, L"MSG_WINDOW_HINTS screen");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowUnmap(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogInfo("Unmapping window 0x%x", window);

    header.type = MSG_UNMAP;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(header, L"MSG_UNMAP");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

// if windowData == 0, use the whole screen
ULONG SendWindowMap(IN const WINDOW_DATA *windowData OPTIONAL)
{
    struct msg_hdr header;
    struct msg_map_info mapMsg;
    ULONG status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    if (windowData)
        LogInfo("Mapping window 0x%x", windowData->WindowHandle);
    else
        LogInfo("Mapping desktop window");

    header.type = MSG_MAP;
    if (windowData)
#pragma warning(suppress:4311)
        header.window = (uint32_t)windowData->WindowHandle;
    else
        header.window = 0;
    header.untrusted_len = 0;

    if (windowData && windowData->ModalParent)
#pragma warning(suppress:4311)
        mapMsg.transient_for = (uint32_t)windowData->ModalParent;
    else
#pragma warning(suppress:4311)
        mapMsg.transient_for = (uint32_t)INVALID_HANDLE_VALUE;

    if (windowData)
        mapMsg.override_redirect = windowData->IsOverrideRedirect;
    else
        mapMsg.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND_MSG(header, mapMsg, L"MSG_MAP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    // if the window takes the whole screen (like logon window), try to make it fullscreen in dom0
    if (!windowData || (windowData->Width == g_ScreenWidth && windowData->Height == g_ScreenHeight))
    {
        status = SendScreenHints(); // min/max screen size
        if (ERROR_SUCCESS != status)
            return status;

        status = SendWindowName(NULL, NULL); // desktop
        if (ERROR_SUCCESS != status)
            return status;

        if (g_ScreenWidth == g_HostScreenWidth && g_ScreenHeight == g_HostScreenHeight)
        {
            LogDebug("fullscreen window");
            status = SendWindowFlags(windowData ? windowData->WindowHandle : NULL, WINDOW_FLAG_FULLSCREEN, 0);
            if (ERROR_SUCCESS != status)
                return status;
        }
    }

    return ERROR_SUCCESS;
}

// if windowData == 0, use the whole screen
ULONG SendWindowConfigure(IN const WINDOW_DATA *windowData OPTIONAL)
{
    struct msg_hdr header;
    struct msg_configure configureMsg;
    struct msg_map_info mapMsg;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    if (windowData)
    {
        LogDebug("0x%x", windowData->WindowHandle);
#pragma warning(suppress:4311)
        header.window = (uint32_t)windowData->WindowHandle;

        header.type = MSG_CONFIGURE;

        configureMsg.x = windowData->X;
        configureMsg.y = windowData->Y;
        configureMsg.width = windowData->Width;
        configureMsg.height = windowData->Height;
        configureMsg.override_redirect = windowData->IsOverrideRedirect;
    }
    else // whole screen
    {
        LogDebug("fullscreen: (0,0) %dx%d", g_ScreenWidth, g_ScreenHeight);
        header.window = 0;

        header.type = MSG_CONFIGURE;

        configureMsg.x = 0;
        configureMsg.y = 0;
        configureMsg.width = g_ScreenWidth;
        configureMsg.height = g_ScreenHeight;
        configureMsg.override_redirect = 0;
    }

    status = TRUE;
    EnterCriticalSection(&g_VchanCriticalSection);

    // don't send resize to 0x0 - this window is just hiding itself, MSG_UNMAP will follow
    if (configureMsg.width > 0 && configureMsg.height > 0)
    {
        status = VCHAN_SEND_MSG(header, configureMsg, L"MSG_CONFIGURE");
        if (!status)
            goto cleanup;
    }

    if (windowData)
    {
#pragma warning(suppress:4311)
        mapMsg.transient_for = (uint32_t)INVALID_HANDLE_VALUE; // TODO?
        mapMsg.override_redirect = windowData->IsOverrideRedirect;

        header.type = MSG_MAP;
        status = VCHAN_SEND_MSG(header, mapMsg, L"MSG_MAP");
    }

cleanup:
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

// Send screen resolution back to gui daemon.
ULONG SendScreenConfigure(IN UINT32 x, IN UINT32 y, IN UINT32 width, IN UINT32 height)
{
    struct msg_hdr header;
    struct msg_configure configMsg;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogDebug("(%d,%d) %dx%d", x, y, width, height);
    header.window = 0; // 0 = screen

    header.type = MSG_CONFIGURE;

    configMsg.x = x;
    configMsg.y = y;
    configMsg.width = width;
    configMsg.height = height;
    configMsg.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, configMsg, L"MSG_CONFIGURE screen");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height)
{
    struct msg_shmimage shmMsg;
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogVerbose("0x%x (%d,%d)-(%d,%d)", window, x, y, x + width, y + height);
    header.type = MSG_SHMIMAGE;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    shmMsg.x = x;
    shmMsg.y = y;
    shmMsg.width = width;
    shmMsg.height = height;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, shmMsg, L"MSG_SHMIMAGE");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL)
{
    struct msg_hdr header;
    struct msg_wmname nameMsg;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    if (window)
    {
        if (caption)
        {
            StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%S", caption);
        }
        else
        {
            if (0 == GetWindowTextA(window, nameMsg.data, RTL_NUMBER_OF(nameMsg.data)))
            {
                perror("GetWindowTextA");
                return ERROR_SUCCESS; // whatever
            }
        }
    }
    else
    {
        StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%s (Windows Desktop)", g_DomainName);
    }

    LogDebug("0x%x %S", window, nameMsg.data);

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.type = MSG_WMNAME;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, nameMsg, L"MSG_WMNAME");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendProtocolVersion(void)
{
    uint32_t version = QUBES_GUI_PROTOCOL_VERSION_WINDOWS;
    BOOL status;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(version, L"version");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}
