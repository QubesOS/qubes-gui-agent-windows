#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include "common.h"

#define QUBES_DRIVER_NAME   L"Qubes Video Driver"

ULONG FindQubesDisplayDevice(
    IN DISPLAY_DEVICE *qubesDisplayDevice
    )
{
    DISPLAY_DEVICE displayDevice = { 0 };
    DWORD devNum = 0;
    BOOL result;

    displayDevice.cb = sizeof(displayDevice);

    devNum = 0;
    while ((result = EnumDisplayDevices(NULL, devNum, &displayDevice, 0)) == TRUE)
    {
        wprintf(L"DevNum:%d\nName:%s\nString:%s\nID:%s\nKey:%s\n\n",
            devNum, &displayDevice.DeviceName[0], &displayDevice.DeviceString[0], &displayDevice.DeviceID[0], &displayDevice.DeviceKey[0]);

        if (wcscmp(&displayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
            break;

        devNum++;
    }

    if (!result)
    {
        wprintf(L"%S: No '%s' found.\n", __FUNCTION__, QUBES_DRIVER_NAME);
        return ERROR_FILE_NOT_FOUND;
    }

    memcpy(qubesDisplayDevice, &displayDevice, sizeof(DISPLAY_DEVICE));

    return ERROR_SUCCESS;
}

ULONG SupportVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN BYTE bpp
    )
{
    HDC controlDC;
    QV_SUPPORT_MODE supportMode;
    int status;

    if (!deviceName)
        return ERROR_INVALID_PARAMETER;

    if (!IS_RESOLUTION_VALID(width, height))
        return ERROR_INVALID_PARAMETER;

    controlDC = CreateDC(NULL, deviceName, NULL, NULL);
    if (!controlDC)
    {
        wprintf(L"%S: Could not create a device context\n", __FUNCTION__);
        return ERROR_FILE_NOT_FOUND;
    }

    supportMode.Magic = QVIDEO_MAGIC;
    supportMode.Width = width;
    supportMode.Height = height;
    supportMode.Bpp = bpp;

    status = ExtEscape(controlDC, QVESC_SUPPORT_MODE, sizeof(supportMode), (char *) &supportMode, 0, NULL);
    DeleteDC(controlDC);

    if (status <= 0)
    {
        wprintf(L"%S: ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n\n", __FUNCTION__, status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
    IN const WCHAR *deviceName,
    IN ULONG width,
    IN ULONG height,
    IN BYTE bpp
    )
{
    DEVMODE devMode = { 0 };
    ULONG status;
    DWORD modeIndex;

    if (!deviceName)
        return ERROR_INVALID_PARAMETER;

    devMode.dmSize = sizeof(devMode);

    // Iterate to get all the available modes of the driver;
    // this will flush the mode cache and force win32k to call our DrvGetModes().
    // Without this, win32k will try to match a specified mode in the cache,
    // will fail and return DISP_CHANGE_BADMODE.
    modeIndex = 0;
    while (EnumDisplaySettings(deviceName, modeIndex, &devMode))
    {
        wprintf(L"Supported mode: %dx%d@%d\n", devMode.dmPelsWidth, devMode.dmPelsHeight, devMode.dmBitsPerPel);
        modeIndex++;
    }

    if (!modeIndex)
    {
        // Couldn't find a single supported video mode.
        wprintf(L"%S: EnumDisplaySettings() failed\n", __FUNCTION__);
        return ERROR_INVALID_FUNCTION;
    }

    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmBitsPerPel = bpp;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

    status = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, CDS_TEST, NULL);
    if (DISP_CHANGE_SUCCESSFUL != status)
    {
        wprintf(L"%S: ChangeDisplaySettingsEx(CDS_TEST) returned %d\n", __FUNCTION__, status);
        return ERROR_NOT_SUPPORTED;
    }

    status = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, 0, NULL);
    if (DISP_CHANGE_SUCCESSFUL != status)
    {
        wprintf(L"%S: ChangeDisplaySettingsEx() returned %d\n", __FUNCTION__, status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

int __cdecl wmain(
    int argc,
    WCHAR *argv[]
    )
{
    DISPLAY_DEVICE displayDevice;
    WCHAR *deviceName = NULL;
    ULONG width;
    ULONG height;
    ULONG status;
    BYTE bpp = 32;

    if (argc < 3)
    {
        wprintf(L"Usage: qvcontrol <width> <height>");
        return ERROR_INVALID_PARAMETER;
    }

    width = _wtoi(argv[1]);
    height = _wtoi(argv[2]);

    if (!IS_RESOLUTION_VALID(width, height))
    {
        wprintf(L"Resolution is invalid: %dx%d\n", width, height);
        return ERROR_INVALID_PARAMETER;
    }

    wprintf(L"New resolution: %dx%d bpp %d\n", width, height, bpp);

    status = FindQubesDisplayDevice(&displayDevice);
    if (ERROR_SUCCESS != status)
    {
        wprintf(L"FindQubesDisplayDevice() failed with error %d\n", status);
        return status;
    }

    deviceName = (WCHAR *) &displayDevice.DeviceName[0];
    wprintf(L"DeviceName: %s\n\n", deviceName);

    status = SupportVideoMode(deviceName, width, height, bpp);
    if (ERROR_SUCCESS != status)
    {
        wprintf(L"SupportVideoMode() failed with error %d\n", status);
        return status;
    }

    status = ChangeVideoMode(deviceName, width, height, bpp);
    if (ERROR_SUCCESS != status)
    {
        wprintf(L"ChangeVideoMode() failed with error %d\n", status);
        return status;
    }

    wprintf(L"Video mode changed successfully.\n");
}
