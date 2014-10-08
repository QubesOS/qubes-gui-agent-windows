#include <windows.h>
#include <strsafe.h>

#include "common.h"
#include "main.h"

#include "log.h"

#define QUBES_DRIVER_NAME L"Qubes Video Driver"

#define CHANGE_DISPLAY_MODE_TRIES 5

BYTE *g_pScreenData = NULL;
HANDLE g_hScreenSection = NULL;

// bit array of dirty pages in the screen buffer (changed since last check)
QV_DIRTY_PAGES *g_pDirtyPages = NULL;
HANDLE g_hDirtySection = NULL;

ULONG CloseScreenSection(void)
{
    LogVerbose("start");

    if (!UnmapViewOfFile(g_pScreenData))
        return perror("UnmapViewOfFile(g_pScreenData)");

    CloseHandle(g_hScreenSection);
    g_pScreenData = NULL;
    g_hScreenSection = NULL;

    if (g_bUseDirtyBits)
    {
        if (!UnmapViewOfFile(g_pDirtyPages))
            return perror("UnmapViewOfFile(g_pDirtyPages)");
        CloseHandle(g_hDirtySection);
        g_pDirtyPages = NULL;
        g_hDirtySection = NULL;
    }

    return ERROR_SUCCESS;
}

ULONG OpenScreenSection()
{
    WCHAR SectionName[100];
    ULONG uLength = g_ScreenHeight * g_ScreenWidth * 4;

    LogVerbose("start");
    // need to explicitly close sections before reopening them
    if (g_hScreenSection && g_pScreenData)
    {
        return ERROR_NOT_SUPPORTED;
    }

    StringCchPrintf(SectionName, RTL_NUMBER_OF(SectionName),
        L"Global\\QubesSharedMemory_%x", uLength);
    LogDebug("screen section: %s", SectionName);

    g_hScreenSection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
    if (!g_hScreenSection)
        return perror("OpenFileMapping(screen section)");

    g_pScreenData = (BYTE *) MapViewOfFile(g_hScreenSection, FILE_MAP_READ, 0, 0, 0);
    if (!g_pScreenData)
        return perror("MapViewOfFile(screen section)");

    if (g_bUseDirtyBits)
    {
        uLength /= PAGE_SIZE;
        StringCchPrintf(SectionName, RTL_NUMBER_OF(SectionName),
            L"Global\\QvideoDirtyPages_%x", sizeof(QV_DIRTY_PAGES) + (uLength >> 3) + 1);
        LogDebug("dirty section: %s", SectionName);

        g_hDirtySection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
        if (!g_hDirtySection)
            return perror("OpenFileMapping(dirty section)");

        g_pDirtyPages = (QV_DIRTY_PAGES *) MapViewOfFile(g_hDirtySection, FILE_MAP_READ, 0, 0, 0);
        if (!g_pDirtyPages)
            return perror("MapViewOfFile(dirty section)");

        LogDebug("dirty section=0x%x, data=0x%x", g_hDirtySection, g_pDirtyPages);
    }

    return ERROR_SUCCESS;
}

ULONG FindQubesDisplayDevice(DISPLAY_DEVICE *pQubesDisplayDevice)
{
    DISPLAY_DEVICE DisplayDevice;
    DWORD iDevNum = 0;
    BOOL bResult;

    LogVerbose("start");

    DisplayDevice.cb = sizeof(DISPLAY_DEVICE);

    iDevNum = 0;
    while ((bResult = EnumDisplayDevices(NULL, iDevNum, &DisplayDevice, 0)) == TRUE)
    {
        LogDebug("DevNum: %d\nName: %s\nString: %s\nFlags: %x\nID: %s\nKey: %s\n",
            iDevNum, &DisplayDevice.DeviceName[0], &DisplayDevice.DeviceString[0],
            DisplayDevice.StateFlags, &DisplayDevice.DeviceID[0], &DisplayDevice.DeviceKey[0]);

        if (_tcscmp(&DisplayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
            break;

        iDevNum++;
    }

    if (!bResult)
    {
        LogError("No '%s' found.\n", QUBES_DRIVER_NAME);
        return ERROR_NOT_SUPPORTED;
    }

    memcpy(pQubesDisplayDevice, &DisplayDevice, sizeof(DISPLAY_DEVICE));

    //debugf("success");
    return ERROR_SUCCESS;
}

// tells qvideo that given resolution will be set by the system
ULONG SupportVideoMode(
    LPTSTR ptszQubesDeviceName,
    ULONG uWidth,
    ULONG uHeight,
    ULONG uBpp
    )
{
    HDC hControlDC;
    QV_SUPPORT_MODE QvSupportMode;
    int iRet;

    LogDebug("%s %dx%d @ %d", ptszQubesDeviceName, uWidth, uHeight, uBpp);
    if (!ptszQubesDeviceName)
        return ERROR_INVALID_PARAMETER;

    if (!IS_RESOLUTION_VALID(uWidth, uHeight))
        return ERROR_INVALID_PARAMETER;

    hControlDC = CreateDC(NULL, ptszQubesDeviceName, NULL, NULL);
    if (!hControlDC)
        return perror("CreateDC");

    QvSupportMode.uMagic = QVIDEO_MAGIC;
    QvSupportMode.uWidth = uWidth;
    QvSupportMode.uHeight = uHeight;
    QvSupportMode.uBpp = uBpp;

    iRet = ExtEscape(hControlDC, QVESC_SUPPORT_MODE, sizeof(QvSupportMode), (LPCSTR) &QvSupportMode, 0, NULL);
    DeleteDC(hControlDC);

    if (iRet <= 0)
    {
        LogError("ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG GetWindowData(
    HWND hWnd,
    QV_GET_SURFACE_DATA_RESPONSE *pQvGetSurfaceDataResponse,
    PFN_ARRAY *pPfnArray
    )
{
    HDC hDC;
    QV_GET_SURFACE_DATA QvGetSurfaceData;
    int iRet;

    LogDebug("hwnd=0x%x, p=%p", hWnd, pQvGetSurfaceDataResponse);
    if (!pQvGetSurfaceDataResponse)
        return ERROR_INVALID_PARAMETER;

    hDC = GetDC(hWnd);
    if (!hDC)
        return perror("GetDC");

    QvGetSurfaceData.uMagic = QVIDEO_MAGIC;
    QvGetSurfaceData.pPfnArray = pPfnArray;

    iRet = ExtEscape(hDC, QVESC_GET_SURFACE_DATA, sizeof(QV_GET_SURFACE_DATA),
        (LPCSTR) &QvGetSurfaceData, sizeof(QV_GET_SURFACE_DATA_RESPONSE), (LPSTR) pQvGetSurfaceDataResponse);

    ReleaseDC(hWnd, hDC);

    if (iRet <= 0)
    {
        LogError("ExtEscape(QVESC_GET_SURFACE_DATA) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    if (QVIDEO_MAGIC != pQvGetSurfaceDataResponse->uMagic)
    {
        LogError("The response to QVESC_GET_SURFACE_DATA is not valid\n");
        return ERROR_NOT_SUPPORTED;
    }

    LogDebug("hdc 0x%0x, IsScreen %d, %dx%d @ %d, delta %d", hDC, pQvGetSurfaceDataResponse->bIsScreen,
        pQvGetSurfaceDataResponse->uWidth, pQvGetSurfaceDataResponse->uHeight,
        pQvGetSurfaceDataResponse->ulBitCount, pQvGetSurfaceDataResponse->lDelta);

    return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
    WCHAR *deviceName,
    ULONG uWidth,
    ULONG uHeight,
    ULONG uBpp
    )
{
    DEVMODE devMode;
    ULONG uResult = ERROR_SUCCESS;
    DWORD iModeNum;
    BOOL bFound = FALSE;
    DWORD iTriesLeft;

    LogInfo("%s %dx%d @ %d", deviceName, uWidth, uHeight, uBpp);
    if (!deviceName)
        return ERROR_INVALID_PARAMETER;

    memset(&devMode, 0, sizeof(DEVMODE));
    devMode.dmSize = sizeof(DEVMODE);

    if (EnumDisplaySettings(deviceName, ENUM_CURRENT_SETTINGS, &devMode))
    {
        if (devMode.dmPelsWidth == uWidth &&
            devMode.dmPelsHeight == uHeight &&
            devMode.dmBitsPerPel == uBpp)
        {
            // the current mode is good
            goto cleanup;
        }
    }
    // Iterate to get all the available modes of the driver;
    // this will flush the mode cache and force win32k to call our DrvGetModes().
    // Without this, win32k will try to match a specified mode in the cache,
    // will fail and return DISP_CHANGE_BADMODE.
    iModeNum = 0;
    while (EnumDisplaySettings(deviceName, iModeNum, &devMode))
    {
        LogDebug("mode %d: %dx%d@%d\n",
            iModeNum, devMode.dmPelsWidth, devMode.dmPelsHeight, devMode.dmBitsPerPel);
        if (devMode.dmPelsWidth == uWidth &&
            devMode.dmPelsHeight == uHeight &&
            devMode.dmBitsPerPel == uBpp)
        {
            bFound = TRUE;
            break;
        }
        iModeNum++;
    }

    if (!bFound)
    {
        LogError("EnumDisplaySettings() didn't return expected mode\n");
        uResult = ERROR_INVALID_FUNCTION;
        goto cleanup;
    }

    // dirty workaround for failing ChangeDisplaySettingsEx when called too early
    iTriesLeft = CHANGE_DISPLAY_MODE_TRIES;
    while (iTriesLeft--)
    {
        uResult = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, CDS_TEST, NULL);
        if (DISP_CHANGE_SUCCESSFUL != uResult)
        {
            LogError("ChangeDisplaySettingsEx(CDS_TEST) failed: %d", uResult);
        }
        else
        {
            uResult = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, 0, NULL);
            if (DISP_CHANGE_SUCCESSFUL != uResult)
                LogError("ChangeDisplaySettingsEx() failed: %d", uResult);
            else
                break;
        }
        Sleep(1000);
    }

    if (DISP_CHANGE_SUCCESSFUL != uResult)
        uResult = ERROR_NOT_SUPPORTED;

cleanup:
    //debugf("end: %d", uResult);
    SetLastError(uResult);
    return uResult;
}

ULONG RegisterWatchedDC(
    HDC hDC,
    HANDLE hModificationEvent
    )
{
    QV_WATCH_SURFACE QvWatchSurface;
    int iRet;

    LogDebug("hdc=0x%x, event=0x%x", hDC, hModificationEvent);
    QvWatchSurface.uMagic = QVIDEO_MAGIC;
    QvWatchSurface.hUserModeEvent = hModificationEvent;

    iRet = ExtEscape(hDC, QVESC_WATCH_SURFACE, sizeof(QV_WATCH_SURFACE), (LPCSTR) &QvWatchSurface, 0, NULL);

    if (iRet <= 0)
    {
        LogError("ExtEscape(QVESC_WATCH_SURFACE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG UnregisterWatchedDC(HDC hDC)
{
    QV_STOP_WATCHING_SURFACE QvStopWatchingSurface;
    int iRet;

    LogDebug("hdc=0x%x", hDC);
    QvStopWatchingSurface.uMagic = QVIDEO_MAGIC;

    iRet = ExtEscape(hDC, QVESC_STOP_WATCHING_SURFACE, sizeof(QV_STOP_WATCHING_SURFACE),
        (LPCSTR) &QvStopWatchingSurface, 0, NULL);

    if (iRet <= 0)
    {
        LogError("ExtEscape(QVESC_STOP_WATCHING_SURFACE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG SynchronizeDirtyBits(HDC hDC)
{
    QV_SYNCHRONIZE qvs;
    int iRet;

    qvs.uMagic = QVIDEO_MAGIC;
    iRet = ExtEscape(hDC, QVESC_SYNCHRONIZE, sizeof(qvs), (LPCSTR) &qvs, 0, NULL);
    if (iRet <= 0)
    {
        LogError("ExtEscape(QVESC_SYNCHRONIZE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}
