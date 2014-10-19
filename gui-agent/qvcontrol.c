#include <windows.h>

#include "common.h"
#include "main.h"

#include "log.h"

#include <strsafe.h>

#define QUBES_DRIVER_NAME L"Qubes Video Driver"

#define CHANGE_DISPLAY_MODE_TRIES 5

BYTE *g_ScreenData = NULL;
HANDLE g_ScreenSection = NULL;

// bit array of dirty pages in the screen buffer (changed since last check)
QV_DIRTY_PAGES *g_DirtyPages = NULL;
HANDLE g_DirtySection = NULL;

ULONG CloseScreenSection(void)
{
    LogVerbose("start");

    if (!UnmapViewOfFile(g_ScreenData))
        return perror("UnmapViewOfFile(g_pScreenData)");

    CloseHandle(g_ScreenSection);
    g_ScreenData = NULL;
    g_ScreenSection = NULL;

    if (g_UseDirtyBits)
    {
        if (!UnmapViewOfFile(g_DirtyPages))
            return perror("UnmapViewOfFile(g_pDirtyPages)");
        CloseHandle(g_DirtySection);
        g_DirtyPages = NULL;
        g_DirtySection = NULL;
    }

    return ERROR_SUCCESS;
}

ULONG OpenScreenSection(void)
{
    WCHAR sectionName[100];
    ULONG bufferSize = g_ScreenHeight * g_ScreenWidth * 4;

    LogVerbose("start");
    // need to explicitly close sections before reopening them
    if (g_ScreenSection && g_ScreenData)
    {
        return ERROR_NOT_SUPPORTED;
    }

    StringCchPrintf(sectionName, RTL_NUMBER_OF(sectionName),
        L"Global\\QubesSharedMemory_%x", bufferSize);
    LogDebug("screen section: %s", sectionName);

    g_ScreenSection = OpenFileMapping(FILE_MAP_READ, FALSE, sectionName);
    if (!g_ScreenSection)
        return perror("OpenFileMapping(screen section)");

    g_ScreenData = (BYTE *) MapViewOfFile(g_ScreenSection, FILE_MAP_READ, 0, 0, 0);
    if (!g_ScreenData)
        return perror("MapViewOfFile(screen section)");

    if (g_UseDirtyBits)
    {
        bufferSize /= PAGE_SIZE;
        StringCchPrintf(sectionName, RTL_NUMBER_OF(sectionName),
            L"Global\\QvideoDirtyPages_%x", sizeof(QV_DIRTY_PAGES) + (bufferSize >> 3) + 1);
        LogDebug("dirty section: %s", sectionName);

        g_DirtySection = OpenFileMapping(FILE_MAP_READ, FALSE, sectionName);
        if (!g_DirtySection)
            return perror("OpenFileMapping(dirty section)");

        g_DirtyPages = (QV_DIRTY_PAGES *) MapViewOfFile(g_DirtySection, FILE_MAP_READ, 0, 0, 0);
        if (!g_DirtyPages)
            return perror("MapViewOfFile(dirty section)");

        LogDebug("dirty section=0x%x, data=0x%x", g_DirtySection, g_DirtyPages);
    }

    return ERROR_SUCCESS;
}

ULONG FindQubesDisplayDevice(
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

    //debugf("success");
    return ERROR_SUCCESS;
}

// tells qvideo that given resolution will be set by the system
ULONG SupportVideoMode(
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
        return perror("CreateDC");

    input.uMagic = QVIDEO_MAGIC;
    input.uWidth = width;
    input.uHeight = height;
    input.uBpp = bpp;

    status = ExtEscape(qvideoDc, QVESC_SUPPORT_MODE, sizeof(input), (char *) &input, 0, NULL);
    DeleteDC(qvideoDc);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG GetWindowData(
    IN HWND window,
    OUT QV_GET_SURFACE_DATA_RESPONSE *surfaceData,
    OUT PFN_ARRAY *pfnArray
    )
{
    HDC qvideoDc;
    QV_GET_SURFACE_DATA input;
    int status;

    LogDebug("hwnd=0x%x, p=%p", window, surfaceData);
    if (!surfaceData)
        return ERROR_INVALID_PARAMETER;

    qvideoDc = GetDC(window);
    if (!qvideoDc)
        return perror("GetDC");

    input.uMagic = QVIDEO_MAGIC;
    input.pPfnArray = pfnArray;

    status = ExtEscape(qvideoDc, QVESC_GET_SURFACE_DATA, sizeof(QV_GET_SURFACE_DATA),
        (LPCSTR) &input, sizeof(QV_GET_SURFACE_DATA_RESPONSE), (char *) surfaceData);

    ReleaseDC(window, qvideoDc);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_GET_SURFACE_DATA) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    if (QVIDEO_MAGIC != surfaceData->uMagic)
    {
        LogError("The response to QVESC_GET_SURFACE_DATA is not valid\n");
        return ERROR_NOT_SUPPORTED;
    }

    LogDebug("hdc 0x%0x, IsScreen %d, %dx%d @ %d, delta %d", qvideoDc, surfaceData->bIsScreen,
        surfaceData->uWidth, surfaceData->uHeight,
        surfaceData->ulBitCount, surfaceData->lDelta);

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

    LogInfo("%s %dx%d @ %d", deviceName, width, height, bpp);
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
    //debugf("end: %d", uResult);
    SetLastError(status);
    return status;
}

ULONG RegisterWatchedDC(
    IN HDC dc,
    IN HANDLE damageEvent
    )
{
    QV_WATCH_SURFACE input;
    int status;

    LogDebug("hdc=0x%x, event=0x%x", dc, damageEvent);
    input.uMagic = QVIDEO_MAGIC;
    input.hUserModeEvent = damageEvent;

    status = ExtEscape(dc, QVESC_WATCH_SURFACE, sizeof(QV_WATCH_SURFACE), (char *) &input, 0, NULL);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_WATCH_SURFACE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG UnregisterWatchedDC(
    IN HDC dc
    )
{
    QV_STOP_WATCHING_SURFACE input;
    int status;

    LogDebug("hdc=0x%x", dc);
    input.uMagic = QVIDEO_MAGIC;

    status = ExtEscape(dc, QVESC_STOP_WATCHING_SURFACE, sizeof(QV_STOP_WATCHING_SURFACE),
        (char *) &input, 0, NULL);

    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_STOP_WATCHING_SURFACE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG SynchronizeDirtyBits(
    IN HDC dc
    )
{
    QV_SYNCHRONIZE input;
    int status;

    input.uMagic = QVIDEO_MAGIC;
    status = ExtEscape(dc, QVESC_SYNCHRONIZE, sizeof(input), (char *) &input, 0, NULL);
    if (status <= 0)
    {
        LogError("ExtEscape(QVESC_SYNCHRONIZE) failed, error %d\n", status);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}
