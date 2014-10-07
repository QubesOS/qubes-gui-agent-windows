#include "qvcontrol.h"

#define QUBES_DRIVER_NAME	_T("Qubes Video Driver")

ULONG FindQubesDisplayDevice(
    PDISPLAY_DEVICE pQubesDisplayDevice
    )
{
    DISPLAY_DEVICE DisplayDevice;
    DWORD iDevNum = 0;
    BOOL bResult;

    memset(&DisplayDevice, 0, sizeof(DISPLAY_DEVICE));

    DisplayDevice.cb = sizeof(DISPLAY_DEVICE);

    iDevNum = 0;
    while ((bResult = EnumDisplayDevices(NULL, iDevNum, &DisplayDevice, 0)) == TRUE)
    {
        _tprintf(_T("DevNum:%d\nName:%s\nString:%s\nID:%s\nKey:%s\n\n"),
            iDevNum, &DisplayDevice.DeviceName[0], &DisplayDevice.DeviceString[0], &DisplayDevice.DeviceID[0], &DisplayDevice.DeviceKey[0]);

        if (_tcscmp(&DisplayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
            break;

        iDevNum++;
    }

    if (!bResult)
    {
        _tprintf(_T(__FUNCTION__) _T("(): No '%s' found.\n"), QUBES_DRIVER_NAME);
        return ERROR_FILE_NOT_FOUND;
    }

    memcpy(pQubesDisplayDevice, &DisplayDevice, sizeof(DISPLAY_DEVICE));

    return ERROR_SUCCESS;
}

ULONG SupportVideoMode(
    LPTSTR ptszQubesDeviceName,
    ULONG uWidth,
    ULONG uHeight,
    UCHAR uBpp
    )
{
    HDC hControlDC;
    QV_SUPPORT_MODE QvSupportMode;
    int iRet;

    if (!ptszQubesDeviceName)
        return ERROR_INVALID_PARAMETER;

    if (!IS_RESOLUTION_VALID(uWidth, uHeight))
        return ERROR_INVALID_PARAMETER;

    hControlDC = CreateDC(NULL, ptszQubesDeviceName, NULL, NULL);
    if (!hControlDC)
    {
        _tprintf(_T(__FUNCTION__) _T("(): Could not create a device context\n"));
        return ERROR_FILE_NOT_FOUND;
    }

    QvSupportMode.uMagic = QVIDEO_MAGIC;
    QvSupportMode.uWidth = uWidth;
    QvSupportMode.uHeight = uHeight;
    QvSupportMode.uBpp = uBpp;

    iRet = ExtEscape(hControlDC, QVESC_SUPPORT_MODE, sizeof(QvSupportMode), (LPCSTR) & QvSupportMode, 0, NULL);
    DeleteDC(hControlDC);

    if (iRet <= 0)
    {
        _tprintf(_T(__FUNCTION__) _T("(): ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n\n"), iRet);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
    LPTSTR ptszDeviceName,
    ULONG uWidth,
    ULONG uHeight,
    UCHAR uBpp
    )
{
    DEVMODE DevMode;
    ULONG uResult;
    DWORD iModeNum;

    if (!ptszDeviceName)
        return ERROR_INVALID_PARAMETER;

    memset(&DevMode, 0, sizeof(DEVMODE));
    DevMode.dmSize = sizeof(DEVMODE);

    // Iterate to get all the available modes of the driver;
    // this will flush the mode cache and force win32k to call our DrvGetModes().
    // Without this, win32k will try to match a specified mode in the cache,
    // will fail and return DISP_CHANGE_BADMODE.
    iModeNum = 0;
    while (EnumDisplaySettings(ptszDeviceName, iModeNum, &DevMode))
    {
        _tprintf(_T("Supported mode: %dx%d@%d\n"), DevMode.dmPelsWidth, DevMode.dmPelsHeight, DevMode.dmBitsPerPel);
        iModeNum++;
    }

    if (!iModeNum)
    {
        // Couldn't find a single supported video mode.
        _tprintf(_T(__FUNCTION__) _T("(): EnumDisplaySettings() failed\n"));
        return ERROR_INVALID_FUNCTION;
    }

    DevMode.dmPelsWidth = uWidth;
    DevMode.dmPelsHeight = uHeight;
    DevMode.dmBitsPerPel = uBpp;
    DevMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

    uResult = ChangeDisplaySettingsEx(ptszDeviceName, &DevMode, NULL, CDS_TEST, NULL);
    if (DISP_CHANGE_SUCCESSFUL != uResult)
    {
        _tprintf(_T(__FUNCTION__) _T("(): ChangeDisplaySettingsEx(CDS_TEST) returned %d\n"), uResult);
        return ERROR_NOT_SUPPORTED;
    }

    uResult = ChangeDisplaySettingsEx(ptszDeviceName, &DevMode, NULL, 0, NULL);
    if (DISP_CHANGE_SUCCESSFUL != uResult)
    {
        _tprintf(_T(__FUNCTION__) _T("(): ChangeDisplaySettingsEx() returned %d\n"), uResult);
        return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

VOID __cdecl _tmain(
    int argc,
    TCHAR * argv[]
    )
{
    DISPLAY_DEVICE DisplayDevice;
    LPTSTR ptszDeviceName = NULL;
    ULONG uWidth;
    ULONG uHeight;
    ULONG uResult;
    UCHAR uBpp = 32;

    if (argc < 3)
    {
        _tprintf(_T("Usage: qvcontrol <width> <height>"));
        return;
    }

    uWidth = _tstoi(argv[1]);
    uHeight = _tstoi(argv[2]);

    if (!IS_RESOLUTION_VALID(uWidth, uHeight))
    {
        _tprintf(_T("Resolution is invalid: %dx%d\n"), uWidth, uHeight);
        return;
    }

    _tprintf(_T("New resolution: %dx%d bpp %d\n"), uWidth, uHeight, uBpp);

    uResult = FindQubesDisplayDevice(&DisplayDevice);
    if (ERROR_SUCCESS != uResult)
    {
        _tprintf(_T("FindQubesDisplayDevice() failed with error %d\n"), uResult);
        return;
    }
    ptszDeviceName = (LPTSTR) & DisplayDevice.DeviceName[0];

    _tprintf(_T("DeviceName: %s\n\n"), ptszDeviceName);

    uResult = SupportVideoMode(ptszDeviceName, uWidth, uHeight, uBpp);
    if (ERROR_SUCCESS != uResult)
    {
        _tprintf(_T("SupportVideoMode() failed with error %d\n"), uResult);
        return;
    }

    uResult = ChangeVideoMode(ptszDeviceName, uWidth, uHeight, uBpp);
    if (ERROR_SUCCESS != uResult)
    {
        _tprintf(_T("ChangeVideoMode() failed with error %d\n"), uResult);
        return;
    }

    _tprintf(_T("Video mode changed successfully.\n"));
}
