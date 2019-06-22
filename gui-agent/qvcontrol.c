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
#include <stdio.h>

#include "common.h"
#include "main.h"

#include <log.h>

#include <strsafe.h>

#define QUBES_DRIVER_NAME L"Qubes Video Driver"

#define CHANGE_DISPLAY_MODE_TRIES 5

// bit array of dirty pages in the screen buffer (changed since last check)
QV_DIRTY_PAGES *g_DirtyPages = NULL;

ULONG QvFindQubesDisplayDevice(
    OUT DISPLAY_DEVICE *qubesDisplayDevice
    )
{
    DISPLAY_DEVICE displayDevice;
    DWORD deviceNum = 0;
    BOOL result;

    LogVerbose("start");

    displayDevice.cb = sizeof(DISPLAY_DEVICE);

    deviceNum = 0;
    while ((result = EnumDisplayDevices(NULL, deviceNum, &displayDevice, 0)) == TRUE)
    {
        LogDebug("DevNum: %d\nName: %s\nString: %s\nFlags: %x\nID: %s\nKey: %s\n",
                 deviceNum, &displayDevice.DeviceName[0], &displayDevice.DeviceString[0],
                 displayDevice.StateFlags, &displayDevice.DeviceID[0], &displayDevice.DeviceKey[0]);

        if (wcscmp(&displayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
            break;

        deviceNum++;
    }

    if (!result)
    {
        LogError("No '%s' found.\n", QUBES_DRIVER_NAME);
        return ERROR_NOT_SUPPORTED;
    }

    memcpy(qubesDisplayDevice, &displayDevice, sizeof(DISPLAY_DEVICE));

    return ERROR_SUCCESS;
}

// tells qvideo that given resolution will be set by the system
ULONG QvSupportVideoMode(
    IN const WCHAR *qubesDisplayDeviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    )
{
    HDC qvideoDc;
    QV_SUPPORT_MODE input;
    int status;

    LogDebug("%s %dx%d @ %d", qubesDisplayDeviceName, width, height, bpp);
    if (!qubesDisplayDeviceName)
        return ERROR_INVALID_PARAMETER;

    if (!IS_RESOLUTION_VALID(width, height))
        return ERROR_INVALID_PARAMETER;

    qvideoDc = CreateDC(NULL, qubesDisplayDeviceName, NULL, NULL);
    if (!qvideoDc)
        return win_perror("CreateDC");

    input.Magic = QVIDEO_MAGIC;
    input.Width = width;
    input.Height = height;
    input.Bpp = bpp;

    status = ExtEscape(qvideoDc, QVESC_SUPPORT_MODE, sizeof(input), (char *)&input, 0, NULL);
    DeleteDC(qvideoDc);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

// maps surface's pfn array into the process
ULONG QvGetWindowData(
    IN HWND window,
    OUT QV_GET_SURFACE_DATA_RESPONSE *surfaceData
    )
{
    HDC qvideoDc;
    QV_GET_SURFACE_DATA input;
    int status;

    LogDebug("hwnd 0x%x", window);
    if (!surfaceData)
        return ERROR_INVALID_PARAMETER;

    qvideoDc = GetDC(window);
    if (!qvideoDc)
        return win_perror("GetDC");

    input.Magic = QVIDEO_MAGIC;

    status = ExtEscape(qvideoDc, QVESC_GET_SURFACE_DATA, sizeof(input), (LPCSTR)&input,
                       sizeof(QV_GET_SURFACE_DATA_RESPONSE), (char *)surfaceData);

    ReleaseDC(window, qvideoDc);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_GET_SURFACE_DATA) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    if (QVIDEO_MAGIC != surfaceData->Magic)
    {
        LogError("The response to QVESC_GET_SURFACE_DATA is not valid\n");
        return ERROR_NOT_SUPPORTED;
    }

    LogDebug("hdc 0x%x, IsScreen %d, %lux%lu @ %lu, stride %lu, pfn array %p, pfns %lu",
             qvideoDc, surfaceData->IsScreen,
             surfaceData->Width, surfaceData->Height,
             surfaceData->Bpp, surfaceData->Stride,
             surfaceData->PfnArray, surfaceData->PfnArray->NumberOf4kPages);

    return ERROR_SUCCESS;
}

// unmap surface's pfn array from the process
ULONG QvReleaseWindowData(
    IN HWND window
    )
{
    HDC qvideoDc;
    QV_RELEASE_SURFACE_DATA input;
    int status;

    LogDebug("hwnd 0x%x", window);

    qvideoDc = GetDC(window);
    if (!qvideoDc)
        return win_perror("GetDC");

    input.Magic = QVIDEO_MAGIC;

    status = ExtEscape(qvideoDc, QVESC_RELEASE_SURFACE_DATA, sizeof(input), (LPCSTR)&input, 0, NULL);

    ReleaseDC(window, qvideoDc);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_RELEASE_SURFACE_DATA) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN ULONG bpp
    )
{
    DEVMODE devMode;
    ULONG status = ERROR_SUCCESS;
    DWORD modeIndex;
    BOOL found = FALSE;
    DWORD triesLeft;

    LogInfo("%s, %dx%d @ %d", deviceName, width, height, bpp);
    if (!deviceName)
        return ERROR_INVALID_PARAMETER;

    ZeroMemory(&devMode, sizeof(DEVMODE));
    devMode.dmSize = sizeof(DEVMODE);

    if (EnumDisplaySettings(deviceName, ENUM_CURRENT_SETTINGS, &devMode))
    {
        if (devMode.dmPelsWidth == width &&
            devMode.dmPelsHeight == height &&
            devMode.dmBitsPerPel == bpp)
        {
            // the current mode is good
            goto cleanup;
        }
    }
    // Iterate to get all the available modes of the driver;
    // this will flush the mode cache and force win32k to call our DrvGetModes().
    // Without this, win32k will try to match a specified mode in the cache,
    // will fail and return DISP_CHANGE_BADMODE.
    modeIndex = 0;
    while (EnumDisplaySettings(deviceName, modeIndex, &devMode))
    {
        LogDebug("mode %d: %dx%d@%d\n",
                 modeIndex, devMode.dmPelsWidth, devMode.dmPelsHeight, devMode.dmBitsPerPel);
        if (devMode.dmPelsWidth == width &&
            devMode.dmPelsHeight == height &&
            devMode.dmBitsPerPel == bpp)
        {
            found = TRUE;
            break;
        }
        modeIndex++;
    }

    if (!found)
    {
        LogError("EnumDisplaySettings() didn't return expected mode\n");
        status = ERROR_INVALID_FUNCTION;
        goto cleanup;
    }

    // dirty workaround for failing ChangeDisplaySettingsEx when called too early
    triesLeft = CHANGE_DISPLAY_MODE_TRIES;
    while (triesLeft--)
    {
        status = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, CDS_TEST, NULL);
        if (DISP_CHANGE_SUCCESSFUL != status)
        {
            LogError("ChangeDisplaySettingsEx(CDS_TEST) failed: 0x%x", status);
        }
        else
        {
            status = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, 0, NULL);
            if (DISP_CHANGE_SUCCESSFUL != status)
                LogError("ChangeDisplaySettingsEx() failed: 0x%x", status);
            else
                break;
        }
        Sleep(1000);
    }

    if (DISP_CHANGE_SUCCESSFUL != status)
        status = ERROR_NOT_SUPPORTED;

cleanup:
    SetLastError(status);
    return status;
}

ULONG QvRegisterWatchedDC(
    IN HDC dc,
    IN HANDLE damageEvent
    )
{
    QV_WATCH_SURFACE input;
    int status;

    LogDebug("hdc 0x%x, event 0x%x", dc, damageEvent);
    input.Magic = QVIDEO_MAGIC;
    input.DamageEvent = damageEvent;

    status = ExtEscape(dc, QVESC_WATCH_SURFACE, sizeof(input), (char *)&input, 0, NULL);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_WATCH_SURFACE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

ULONG QvUnregisterWatchedDC(
    IN HDC dc
    )
{
    QV_STOP_WATCHING_SURFACE input;
    int status;

    LogDebug("hdc=0x%x", dc);
    input.Magic = QVIDEO_MAGIC;

    status = ExtEscape(dc, QVESC_STOP_WATCHING_SURFACE, sizeof(input), (char *)&input, 0, NULL);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_STOP_WATCHING_SURFACE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

ULONG QvSynchronizeDirtyBits(
    IN HDC dc
    )
{
    QV_SYNCHRONIZE input;
    int status;

    input.Magic = QVIDEO_MAGIC;
    status = ExtEscape(dc, QVESC_SYNCHRONIZE, sizeof(input), (char *)&input, 0, NULL);
    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_SYNCHRONIZE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}
