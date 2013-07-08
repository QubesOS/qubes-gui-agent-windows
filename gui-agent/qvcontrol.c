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
	while ((bResult = EnumDisplayDevices(NULL, iDevNum, &DisplayDevice, 0)) == TRUE) {

		_tprintf(_T("DevNum:%d\nName:%s\nString:%s\nID:%s\nKey:%s\n\n"),
			 iDevNum, &DisplayDevice.DeviceName[0], &DisplayDevice.DeviceString[0], &DisplayDevice.DeviceID[0], &DisplayDevice.DeviceKey[0]);

		if (_tcscmp(&DisplayDevice.DeviceString[0], QUBES_DRIVER_NAME) == 0)
			break;

		iDevNum++;
	}

	if (!bResult) {
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
	ULONG uBpp
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
	if (!hControlDC) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): Could not create a device context\n"));
		return ERROR_FILE_NOT_FOUND;
	}

	QvSupportMode.uMagic = QVIDEO_MAGIC;
	QvSupportMode.uWidth = uWidth;
	QvSupportMode.uHeight = uHeight;
	QvSupportMode.uBpp = 32;

	iRet = ExtEscape(hControlDC, QVESC_SUPPORT_MODE, sizeof(QvSupportMode), (LPCSTR) & QvSupportMode, 0, NULL);
	DeleteDC(hControlDC);

	if (iRet <= 0) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ExtEscape(QVESC_SUPPORT_MODE) failed, error %d\n\n"), iRet);
		return ERROR_NOT_SUPPORTED;
	}

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

	if (!pQvGetSurfaceDataResponse)
		return ERROR_INVALID_PARAMETER;

	hDC = GetDC(hWnd);
	if (!hDC) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): Could not get a device context\n"));
		return ERROR_FILE_NOT_FOUND;
	}

	QvGetSurfaceData.uMagic = QVIDEO_MAGIC;
	memset(pQvGetSurfaceDataResponse, 0, sizeof(QV_GET_SURFACE_DATA_RESPONSE));

	iRet = ExtEscape(hDC,
			 QVESC_GET_SURFACE_DATA,
			 sizeof(QV_GET_SURFACE_DATA), (LPCSTR) & QvGetSurfaceData, sizeof(QV_GET_SURFACE_DATA_RESPONSE), (LPSTR) pQvGetSurfaceDataResponse);

	ReleaseDC(hWnd, hDC);

	if (iRet <= 0) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ExtEscape(QVESC_GET_SURFACE_DATA) failed, error %d\n\n"), iRet);
		return ERROR_NOT_SUPPORTED;
	}

	if (QVIDEO_MAGIC != pQvGetSurfaceDataResponse->uMagic) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): The response to QVESC_GET_SURFACE_DATA is not valid\n\n"));
		return ERROR_NOT_SUPPORTED;
	}

	return ERROR_SUCCESS;
}

ULONG ChangeVideoMode(
	LPTSTR ptszDeviceName,
	ULONG uWidth,
	ULONG uHeight,
	ULONG uBpp
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
		iModeNum++;

	if (!iModeNum) {
		// Couldn't find a single supported video mode.
		_tprintf(_T(__FUNCTION__) _T("(): EnumDisplaySettings() failed\n"));
		return ERROR_INVALID_FUNCTION;
	}

	DevMode.dmPelsWidth = uWidth;
	DevMode.dmPelsHeight = uHeight;
	DevMode.dmBitsPerPel = 32;
	DevMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

	uResult = ChangeDisplaySettingsEx(ptszDeviceName, &DevMode, NULL, CDS_TEST, NULL);
	if (DISP_CHANGE_SUCCESSFUL != uResult) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ChangeDisplaySettingsEx(CDS_TEST) returned %d\n"), uResult);
		return ERROR_NOT_SUPPORTED;
	}

	uResult = ChangeDisplaySettingsEx(ptszDeviceName, &DevMode, NULL, 0, NULL);
	if (DISP_CHANGE_SUCCESSFUL != uResult) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ChangeDisplaySettingsEx() returned %d\n"), uResult);
		return ERROR_NOT_SUPPORTED;
	}

	return ERROR_SUCCESS;
}

ULONG RegisterWatchedDC(
	HDC hDC,
	HANDLE hModificationEvent
)
{
	QV_WATCH_SURFACE QvWatchSurface;
	int iRet;

	QvWatchSurface.uMagic = QVIDEO_MAGIC;
	QvWatchSurface.hUserModeEvent = hModificationEvent;

	iRet = ExtEscape(hDC, QVESC_WATCH_SURFACE, sizeof(QV_WATCH_SURFACE), (LPCSTR) & QvWatchSurface, 0, NULL);

	if (iRet <= 0) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ExtEscape(QVESC_WATCH_SURFACE) failed, error %d\n\n"), iRet);
		return ERROR_NOT_SUPPORTED;
	}

	return ERROR_SUCCESS;

}

ULONG UnregisterWatchedDC(
	HDC hDC
)
{
	QV_STOP_WATCHING_SURFACE QvStopWatchingSurface;
	int iRet;

	QvStopWatchingSurface.uMagic = QVIDEO_MAGIC;

	iRet = ExtEscape(hDC, QVESC_STOP_WATCHING_SURFACE, sizeof(QV_STOP_WATCHING_SURFACE), (LPCSTR) & QvStopWatchingSurface, 0, NULL);

	if (iRet <= 0) {
		_tprintf(_T(__FUNCTION__)
			 _T("(): ExtEscape(QVESC_STOP_WATCHING_SURFACE) failed, error %d\n\n"), iRet);
		return ERROR_NOT_SUPPORTED;
	}

	return ERROR_SUCCESS;

}
