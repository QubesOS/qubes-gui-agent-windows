#include "qvcontrol.h"
#include "log.h"

#define QUBES_DRIVER_NAME L"Qubes Video Driver"

#define CHANGE_DISPLAY_MODE_TRIES 5

PBYTE g_pScreenData = NULL;
HANDLE g_hScreenSection = NULL;

// bit array of dirty pages in the screen buffer (changed since last check)
PQV_DIRTY_PAGES g_pDirtyPages = NULL;
HANDLE g_hDirtySection = NULL;

extern BOOL g_bUseDirtyBits;
extern LONG g_ScreenHeight;
extern LONG g_ScreenWidth;

ULONG CloseScreenSection()
{
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

    // need to explicitly close sections before reopening them
    if (g_hScreenSection && g_pScreenData)
    {
        return ERROR_NOT_SUPPORTED;
    }

    StringCchPrintf(SectionName, _countof(SectionName),
        L"Global\\QubesSharedMemory_%x", uLength);
    debugf("screen section: %s", SectionName);

    g_hScreenSection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
    if (!g_hScreenSection)
        return perror("OpenFileMapping(screen section)");

    g_pScreenData = (PBYTE) MapViewOfFile(g_hScreenSection, FILE_MAP_READ, 0, 0, 0);
    if (!g_pScreenData)
        return perror("MapViewOfFile(screen section)");

    if (g_bUseDirtyBits)
    {
        uLength /= PAGE_SIZE;
        StringCchPrintf(SectionName, _countof(SectionName),
            L"Global\\QvideoDirtyPages_%x", sizeof(QV_DIRTY_PAGES) + (uLength >> 3) + 1);
        debugf("dirty section: %s", SectionName);

        g_hDirtySection = OpenFileMapping(FILE_MAP_READ, FALSE, SectionName);
        if (!g_hDirtySection)
            return perror("OpenFileMapping(dirty section)");

        g_pDirtyPages = (PQV_DIRTY_PAGES) MapViewOfFile(g_hDirtySection, FILE_MAP_READ, 0, 0, 0);
        if (!g_pDirtyPages)
            return perror("MapViewOfFile(dirty section)");

        debugf("dirty section=0x%x, data=0x%x", g_hDirtySection, g_pDirtyPages);
    }

    return ERROR_SUCCESS;
}

ULONG FindQubesDisplayDevice(
    PDISPLAY_DEVICE pQubesDisplayDevice
    )
{
    DISPLAY_DEVICE DisplayDevice;
    DWORD iDevNum = 0;
    BOOL bResult;

    debugf("start");

    DisplayDevice.cb = sizeof(DISPLAY_DEVICE);

    iDevNum = 0;
    while ((bResult = EnumDisplayDevices(NULL, iDevNum, &DisplayDevice, 0)) == TRUE)
    {
        logf("DevNum: %d\nName: %s\nString: %s\nFlags: %x\nID: %s\nKey: %s\n",
            iDevNum, &DisplayDevice.DeviceName[0], &DisplayDevice.DeviceString[0],
            DisplayDevice.StateFlags, &DisplayDevice.DeviceID[0], &DisplayDevice.DeviceKey[0]);

        if (_tcscmp(&DisplayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
            break;

        iDevNum++;
    }

    if (!bResult)
    {
        errorf("No '%s' found.\n", QUBES_DRIVER_NAME);
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

    debugf("%s %dx%d @ %d", ptszQubesDeviceName, uWidth, uHeight, uBpp);
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

    iRet = ExtEscape(hControlDC, QVESC_SUPPORT_MODE, sizeof(QvSupportMode), (LPCSTR) & QvSupportMode, 0, NULL);
    DeleteDC(hControlDC);

    if (iRet <= 0)
    {
        errorf("ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG GetWindowData(
    HWND hWnd,
    PQV_GET_SURFACE_DATA_RESPONSE pQvGetSurfaceDataResponse
    )
{
    HDC hDC;
    QV_GET_SURFACE_DATA QvGetSurfaceData;
    int iRet;

    debugf("hwnd=0x%x, p=%p", hWnd, pQvGetSurfaceDataResponse);
    if (!pQvGetSurfaceDataResponse)
        return ERROR_INVALID_PARAMETER;

    hDC = GetDC(hWnd);
    if (!hDC)
        return perror("GetDC");

    QvGetSurfaceData.uMagic = QVIDEO_MAGIC;

    iRet = ExtEscape(hDC, QVESC_GET_SURFACE_DATA, sizeof(QV_GET_SURFACE_DATA),
        (LPCSTR) & QvGetSurfaceData, sizeof(QV_GET_SURFACE_DATA_RESPONSE), (LPSTR) pQvGetSurfaceDataResponse);

    ReleaseDC(hWnd, hDC);

    if (iRet <= 0)
    {
        errorf("ExtEscape(QVESC_GET_SURFACE_DATA) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    if (QVIDEO_MAGIC != pQvGetSurfaceDataResponse->uMagic)
    {
        errorf("The response to QVESC_GET_SURFACE_DATA is not valid\n");
        return ERROR_NOT_SUPPORTED;
    }

    debugf("hdc 0x%0x, IsScreen %d, %dx%d@%d, delta %d", hDC, pQvGetSurfaceDataResponse->bIsScreen,
        pQvGetSurfaceDataResponse->cx, pQvGetSurfaceDataResponse->cy,
        pQvGetSurfaceDataResponse->ulBitCount, pQvGetSurfaceDataResponse->lDelta);

    return ERROR_SUCCESS;
}

ULONG GetPfnList(
    PVOID pVirtualAddress,
    ULONG uRegionSize,
    PPFN_ARRAY pPfnArray
    )
{
    HDC hDC;
    QV_GET_PFN_LIST QvGetPfnList;
    PQV_GET_PFN_LIST_RESPONSE pQvGetPfnListResponse = NULL;
    int iRet;
    DWORD retval = ERROR_SUCCESS;

    debugf("start");
    if (!pVirtualAddress || !uRegionSize || !pPfnArray)
    {
        retval = ERROR_INVALID_PARAMETER;
        goto cleanup;
    }

    pQvGetPfnListResponse = (PQV_GET_PFN_LIST_RESPONSE) malloc(sizeof(QV_GET_PFN_LIST_RESPONSE));
    if (!pQvGetPfnListResponse)
    {
        retval = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    hDC = GetDC(0);
    if (!hDC)
    {
        retval = perror("GetDC");
        goto cleanup;
    }

    QvGetPfnList.uMagic = QVIDEO_MAGIC;
    QvGetPfnList.pVirtualAddress = pVirtualAddress;
    QvGetPfnList.uRegionSize = uRegionSize;

    iRet = ExtEscape(hDC, QVESC_GET_PFN_LIST, sizeof(QV_GET_PFN_LIST),
        (LPCSTR) & QvGetPfnList, sizeof(QV_GET_PFN_LIST_RESPONSE), (LPSTR) pQvGetPfnListResponse);

    ReleaseDC(0, hDC);

    if (iRet <= 0)
    {
        errorf("ExtEscape(QVESC_GET_PFN_LIST) failed, error %d\n", iRet);
        retval = ERROR_NOT_SUPPORTED;
        goto cleanup;
    }

    if (QVIDEO_MAGIC != pQvGetPfnListResponse->uMagic)
    {
        errorf("The response to QVESC_GET_PFN_LIST is not valid\n");
        retval = ERROR_NOT_SUPPORTED;
        goto cleanup;
    }

    memcpy(pPfnArray, &pQvGetPfnListResponse->PfnArray, sizeof(PFN_ARRAY));

cleanup:
    free(pQvGetPfnListResponse);
    return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
    PWCHAR deviceName,
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

    debugf("%s %dx%d @ %d", deviceName, uWidth, uHeight, uBpp);
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
        logf("mode %d: %dx%d@%d\n",
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
        errorf("EnumDisplaySettings() didn't return expected mode\n");
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
            errorf("ChangeDisplaySettingsEx(CDS_TEST) failed: %d", uResult);
        }
        else
        {
            uResult = ChangeDisplaySettingsEx(deviceName, &devMode, NULL, 0, NULL);
            if (DISP_CHANGE_SUCCESSFUL != uResult)
                errorf("ChangeDisplaySettingsEx() failed: %d", uResult);
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

    debugf("hdc=0x%x, event=0x%x", hDC, hModificationEvent);
    QvWatchSurface.uMagic = QVIDEO_MAGIC;
    QvWatchSurface.hUserModeEvent = hModificationEvent;

    iRet = ExtEscape(hDC, QVESC_WATCH_SURFACE, sizeof(QV_WATCH_SURFACE), (LPCSTR) & QvWatchSurface, 0, NULL);

    if (iRet <= 0)
    {
        errorf("ExtEscape(QVESC_WATCH_SURFACE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG UnregisterWatchedDC(HDC hDC)
{
    QV_STOP_WATCHING_SURFACE QvStopWatchingSurface;
    int iRet;

    debugf("hdc=0x%x", hDC);
    QvStopWatchingSurface.uMagic = QVIDEO_MAGIC;

    iRet = ExtEscape(hDC, QVESC_STOP_WATCHING_SURFACE, sizeof(QV_STOP_WATCHING_SURFACE),
        (LPCSTR) & QvStopWatchingSurface, 0, NULL);

    if (iRet <= 0)
    {
        errorf("ExtEscape(QVESC_STOP_WATCHING_SURFACE) failed, error %d\n", iRet);
        return ERROR_NOT_SUPPORTED;
    }

    //debugf("success");
    return ERROR_SUCCESS;
}
