#include "driver.h"

// The driver function table with all function index/address pairs

static DRVFN g_DrvFunctions[] = {
	{INDEX_DrvGetModes, (PFN) DrvGetModes},
	{INDEX_DrvSynchronizeSurface, (PFN) DrvSynchronizeSurface},
	{INDEX_DrvEnablePDEV, (PFN) DrvEnablePDEV},
	{INDEX_DrvCompletePDEV, (PFN) DrvCompletePDEV},
	{INDEX_DrvDisablePDEV, (PFN) DrvDisablePDEV},
	{INDEX_DrvEnableSurface, (PFN) DrvEnableSurface},
	{INDEX_DrvDisableSurface, (PFN) DrvDisableSurface},
	{INDEX_DrvAssertMode, (PFN) DrvAssertMode},
//      {INDEX_DrvCreateDeviceBitmap, (PFN) DrvCreateDeviceBitmap},
//      {INDEX_DrvDeleteDeviceBitmap, (PFN) DrvDeleteDeviceBitmap},
	{INDEX_DrvEscape, (PFN) DrvEscape}
};

#if DBG
LONG g_lCounter = 0;
#endif

ULONG g_uWidth = 800;
ULONG g_uHeight = 600;
ULONG g_uBpp = 32;

#define flGlobalHooks HOOK_SYNCHRONIZE

/******************************Public*Routine******************************\
* DrvEnableDriver
*
* Enables the driver by retrieving the drivers function table and version.
*
\**************************************************************************/
BOOL APIENTRY DrvEnableDriver(
	ULONG iEngineVersion,
	ULONG cj,
	__in_bcount(cj) DRVENABLEDATA * pded
)
{
	DISPDBG((0, "DrvEnableDriver\n"));

	if ((iEngineVersion < DDI_DRIVER_VERSION_NT5) || (cj < sizeof(DRVENABLEDATA))) {
		DISPDBG((0, "DrvEnableDriver(): Unsupported engine version %d, DRVENABLEDATA size %d\n", iEngineVersion, cj));
		return FALSE;
	}

	pded->pdrvfn = g_DrvFunctions;
	pded->c = sizeof(g_DrvFunctions) / sizeof(DRVFN);
	pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;

	return TRUE;
}

/******************************Public*Routine******************************\
* DrvEnablePDEV
*
* DDI function, Enables the Physical Device.
*
* Return Value: device handle to pdev.
*
\**************************************************************************/

DHPDEV APIENTRY DrvEnablePDEV(
	__in DEVMODEW * pDevmode,	// Pointer to DEVMODE
	__in_opt PWSTR pwszLogAddress,	// Logical address
	__in ULONG cPatterns,	// number of patterns
	__in_opt HSURF * ahsurfPatterns,	// return standard patterns
	__in ULONG cjGdiInfo,	// Length of memory pointed to by pGdiInfo
	__out_bcount(cjGdiInfo) ULONG * pGdiInfo,	// Pointer to GdiInfo structure
	__in ULONG cjDevInfo,	// Length of following PDEVINFO structure
	__out_bcount(cjDevInfo) DEVINFO * pDevInfo,	// physical device information structure
	__in_opt HDEV hdev,	// HDEV, used for callbacks
	__in_opt PWSTR pwszDeviceName,	// DeviceName - not used
	__in HANDLE hDriver	// Handle to base driver
)
{
	PPDEV ppdev = (PPDEV) NULL;

	DISPDBG((0, "DrvEnablePDEV\n"));

	UNREFERENCED_PARAMETER(pwszLogAddress);
	UNREFERENCED_PARAMETER(cPatterns);
	UNREFERENCED_PARAMETER(ahsurfPatterns);
	UNREFERENCED_PARAMETER(hdev);
	UNREFERENCED_PARAMETER(pwszDeviceName);

	if (sizeof(DEVINFO) > cjDevInfo) {
		DISPDBG((0, "DrvEnablePDEV(): insufficient pDevInfo memory\n"));
		return NULL;
	}

	if (sizeof(GDIINFO) > cjGdiInfo) {
		DISPDBG((0, "DrvEnablePDEV(): insufficient pGdiInfo memory\n"));
		return NULL;
	}
	// Allocate a physical device structure.

	ppdev = (PPDEV) EngAllocMem(FL_ZERO_MEMORY, sizeof(PDEV), ALLOC_TAG);
	if (ppdev == (PPDEV) NULL) {
		DISPDBG((0, "DrvEnablePDEV(): EngAllocMem() failed\n"));
		return NULL;
	}
	// Save the screen handle in the PDEV.

	ppdev->hDriver = hDriver;

	// Get the current screen mode information. Set up device caps and devinfo.

	if (!bInitPDEV(ppdev, pDevmode, (PGDIINFO) pGdiInfo, pDevInfo)) {
		DISPDBG((0, "DrvEnablePDEV(): bInitPDEV() failed\n"));
		EngFreeMem(ppdev);
		return NULL;
	}

	return (DHPDEV) ppdev;

}

/******************************Public*Routine******************************\
* DrvCompletePDEV
*
* Store the HPDEV, the engines handle for this PDEV, in the DHPDEV.
*
\**************************************************************************/

VOID APIENTRY DrvCompletePDEV(
	DHPDEV dhpdev,
	HDEV hdev
)
{
	DISPDBG((1, "DrvCompletePDEV\n"));
	((PPDEV) dhpdev)->hdevEng = hdev;
}

ULONG APIENTRY DrvGetModes(
	HANDLE hDriver,
	ULONG cjSize,
	DEVMODEW * pdm
)
{
	ULONG ulBytesWritten = 0, ulBytesNeeded = sizeof(DEVMODEW);
	ULONG ulReturnValue;

	UNREFERENCED_PARAMETER(hDriver);
	UNREFERENCED_PARAMETER(cjSize);

	DISPDBG((1, "DrvGetModes\n"));
	if (pdm == NULL) {
		ulReturnValue = ulBytesNeeded;
	} else {

		ulBytesWritten = sizeof(DEVMODEW);

		memset(pdm, 0, sizeof(DEVMODEW));
		memcpy(pdm->dmDeviceName, DLL_NAME, sizeof(DLL_NAME));

		pdm->dmSpecVersion = DM_SPECVERSION;
		pdm->dmDriverVersion = DM_SPECVERSION;

		pdm->dmDriverExtra = 0;
		pdm->dmSize = sizeof(DEVMODEW);
		pdm->dmBitsPerPel = g_uBpp;
		pdm->dmPelsWidth = g_uWidth;
		pdm->dmPelsHeight = g_uHeight;
		pdm->dmDisplayFrequency = 75;

		pdm->dmDisplayFlags = 0;

		pdm->dmPanningWidth = pdm->dmPelsWidth;
		pdm->dmPanningHeight = pdm->dmPelsHeight;

		pdm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;

		ulReturnValue = ulBytesWritten;

	}

	return ulReturnValue;
}

/******************************Public*Routine******************************\
* DrvDisablePDEV
*
* Release the resources allocated in DrvEnablePDEV. If a surface has been
* enabled DrvDisableSurface will have already been called.
*
\**************************************************************************/

VOID APIENTRY DrvDisablePDEV(
	DHPDEV dhpdev
)
{
	PPDEV ppdev = (PPDEV) dhpdev;

	DISPDBG((1, "DrvDisablePDEV\n"));

	EngDeletePalette(ppdev->hpalDefault);
	EngFreeMem(ppdev);
}

ULONG AllocateMemory(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor,
	ULONG uLength
)
{
	PQVMINI_ALLOCATE_MEMORY_RESPONSE pQvminiAllocateMemoryResponse = NULL;
	QVMINI_ALLOCATE_MEMORY QvminiAllocateMemory;
	ULONG nbReturned;
	DWORD dwResult;

	pQvminiAllocateMemoryResponse = (PQVMINI_ALLOCATE_MEMORY_RESPONSE) EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE), ALLOC_TAG);
	if (!pQvminiAllocateMemoryResponse) {
		RIP("AllocateMemory(): EngAllocMem(QVMINI_ALLOCATE_MEMORY_RESPONSE) failed\n");
		return STATUS_NO_MEMORY;
	}

	QvminiAllocateMemory.uLength = uLength;

	dwResult = EngDeviceIoControl(pSurfaceDescriptor->ppdev->hDriver,
				      IOCTL_QVMINI_ALLOCATE_MEMORY,
				      &QvminiAllocateMemory,
				      sizeof(QvminiAllocateMemory), pQvminiAllocateMemoryResponse, sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE), &nbReturned);

	if (0 != dwResult) {
		DISPDBG((0, "AllocateMemory(): EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_MEMORY) failed with error %d\n", dwResult));
		EngFreeMem(pQvminiAllocateMemoryResponse);
		return STATUS_NO_MEMORY;
	}

	pSurfaceDescriptor->pSurfaceData = pQvminiAllocateMemoryResponse->pVirtualAddress;
	memcpy(&pSurfaceDescriptor->PfnArray, &pQvminiAllocateMemoryResponse->PfnArray, sizeof(PFN_ARRAY));

	EngFreeMem(pQvminiAllocateMemoryResponse);

	return 0;
}

ULONG AllocateSection(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor,
	ULONG uLength
)
{
	PQVMINI_ALLOCATE_SECTION_RESPONSE pQvminiAllocateSectionResponse = NULL;
	QVMINI_ALLOCATE_SECTION QvminiAllocateSection;
	ULONG nbReturned;
	DWORD dwResult;

	pQvminiAllocateSectionResponse = (PQVMINI_ALLOCATE_SECTION_RESPONSE) EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE), ALLOC_TAG);
	if (!pQvminiAllocateSectionResponse) {
		RIP("AllocateSurfaceData(): EngAllocMem(QVMINI_ALLOCATE_SECTION_RESPONSE) failed\n");
		return STATUS_NO_MEMORY;
	}

	QvminiAllocateSection.uLength = uLength;

	dwResult = EngDeviceIoControl(pSurfaceDescriptor->ppdev->hDriver,
				      IOCTL_QVMINI_ALLOCATE_SECTION,
				      &QvminiAllocateSection,
				      sizeof(QvminiAllocateSection), pQvminiAllocateSectionResponse, sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE), &nbReturned);

	if (0 != dwResult) {
		DISPDBG((0, "AllocateSurfaceData(): EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_SECTION) failed with error %d\n", dwResult));
		EngFreeMem(pQvminiAllocateSectionResponse);
		return STATUS_NO_MEMORY;
	}

	pSurfaceDescriptor->pSurfaceData = pQvminiAllocateSectionResponse->pVirtualAddress;
	pSurfaceDescriptor->hSection = pQvminiAllocateSectionResponse->hSection;
	pSurfaceDescriptor->SectionObject = pQvminiAllocateSectionResponse->SectionObject;
	pSurfaceDescriptor->pMdl = pQvminiAllocateSectionResponse->pMdl;

	memcpy(&pSurfaceDescriptor->PfnArray, &pQvminiAllocateSectionResponse->PfnArray, sizeof(PFN_ARRAY));

	EngFreeMem(pQvminiAllocateSectionResponse);

	return 0;
}

ULONG AllocateSurfaceData(
	BOOLEAN bSurface,
	PSURFACE_DESCRIPTOR pSurfaceDescriptor,
	ULONG uLength
)
{
	if (bSurface)
		return AllocateSection(pSurfaceDescriptor, uLength);
	else
		return AllocateMemory(pSurfaceDescriptor, uLength);
}

VOID FreeMemory(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor
)
{
	DWORD dwResult;
	QVMINI_FREE_MEMORY QvminiFreeMemory;
	ULONG nbReturned;

	DISPDBG((0, "FreeSurfaceData(%p)\n", pSurfaceDescriptor->pSurfaceData));

	QvminiFreeMemory.pVirtualAddress = pSurfaceDescriptor->pSurfaceData;

	dwResult = EngDeviceIoControl(pSurfaceDescriptor->ppdev->hDriver,
				      IOCTL_QVMINI_FREE_MEMORY, &QvminiFreeMemory, sizeof(QvminiFreeMemory), NULL, 0, &nbReturned);

	if (0 != dwResult) {
		DISPDBG((0, "FreeSurfaceData(): EngDeviceIoControl(IOCTL_QVMINI_FREE_MEMORY) failed with error %d\n", dwResult));
	}
}

VOID FreeSection(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor
)
{
	DWORD dwResult;
	QVMINI_FREE_SECTION QvminiFreeSection;
	ULONG nbReturned;

	DISPDBG((0, "FreeSection(%p)\n", pSurfaceDescriptor->pSurfaceData));

	QvminiFreeSection.pVirtualAddress = pSurfaceDescriptor->pSurfaceData;
	QvminiFreeSection.hSection = pSurfaceDescriptor->hSection;
	QvminiFreeSection.SectionObject = pSurfaceDescriptor->SectionObject;
	QvminiFreeSection.pMdl = pSurfaceDescriptor->pMdl;

	dwResult = EngDeviceIoControl(pSurfaceDescriptor->ppdev->hDriver,
				      IOCTL_QVMINI_FREE_SECTION, &QvminiFreeSection, sizeof(QvminiFreeSection), NULL, 0, &nbReturned);

	if (0 != dwResult) {
		DISPDBG((0, "FreeSection(): EngDeviceIoControl(IOCTL_QVMINI_FREE_SECTION) failed with error %d\n", dwResult));
	}
}

VOID FreeSurfaceData(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor
)
{
	if (pSurfaceDescriptor->bIsScreen)
		FreeSection(pSurfaceDescriptor);
	else
		FreeMemory(pSurfaceDescriptor);
}

VOID FreeSurfaceDescriptor(
	PSURFACE_DESCRIPTOR pSurfaceDescriptor
)
{
	if (!pSurfaceDescriptor)
		return;

	if (pSurfaceDescriptor->hDriverObj)
		// Require a cleanup callback to be called.
		// pSurfaceDescriptor->hDriverObj will be set no NULL by the callback.
		EngDeleteDriverObj(pSurfaceDescriptor->hDriverObj, TRUE, FALSE);

	FreeSurfaceData(pSurfaceDescriptor);
	EngFreeMem(pSurfaceDescriptor);
}

ULONG AllocateNonOpaqueDeviceSurfaceOrBitmap(
	BOOLEAN bSurface,
	HDEV hdev,
	ULONG ulBitCount,
	SIZEL sizl,
	ULONG ulHooks,
	HSURF * pHsurf,
	PSURFACE_DESCRIPTOR * ppSurfaceDescriptor,
	PPDEV ppdev
)
{
	ULONG ulBitmapType;
	ULONG uSurfaceMemorySize;
	PSURFACE_DESCRIPTOR pSurfaceDescriptor;
	ULONG uStride;
	HSURF hsurf;
	DHSURF dhsurf;
	ULONG uResult;
/*	BITMAP_HEADER BitmapHeader = { {0x4D42, 0, 0, 0, 0x8A}
	,
	{0x7C, 0, 0, 1, 32, 3, 0, 0, 0, 0, 0,
	 0x0FF0000, 0x0FF00, 0x0FF, 0x0FF000000, 0x73524742,
	 {{0x28F5C280, 0x151EB860, 0x1EB8520}
	  , {0x13333340, 0x26666680, 0x6666640}
	  , {0x99999A0, 0x3D70A3C, 0x328F5C24}
	  }
	 ,
	 0, 0, 0, 4, 0, 0, 0}
	};
*/
	if (!hdev || !pHsurf || !ppSurfaceDescriptor)
		return STATUS_INVALID_PARAMETER;

	switch (ulBitCount) {
	case 8:
		ulBitmapType = BMF_8BPP;
		break;
	case 16:
		ulBitmapType = BMF_16BPP;
		break;
	case 24:
		ulBitmapType = BMF_24BPP;
		break;
	case 32:
		ulBitmapType = BMF_32BPP;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	uStride = (ulBitCount >> 3) * sizl.cx;

	uSurfaceMemorySize = (ULONG) (uStride * sizl.cy);

//      DISPDBG((0, "AllocateNonOpaqueDeviceSurfaceOrBitmap(): Allocating %dx%d, %d\n", sizl.cx, sizl.cy, ulBitCount));

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) EngAllocMem(FL_ZERO_MEMORY, sizeof(SURFACE_DESCRIPTOR), ALLOC_TAG);
	if (!pSurfaceDescriptor) {
		RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngAllocMem(SURFACE_DESCRIPTOR) failed\n");
		return STATUS_NO_MEMORY;
	}

	pSurfaceDescriptor->ppdev = ppdev;

	// pSurfaceDescriptor->ppdev must be set before calling this,
	// because we query our miniport through EngDeviceIoControl().
	uResult = AllocateSurfaceData(bSurface, pSurfaceDescriptor, uSurfaceMemorySize);
	if (0 != uResult) {
		DISPDBG((0, "AllocateNonOpaqueDeviceSurfaceOrBitmap(): AllocateSurfaceData(%d) failed with error %d\n", uSurfaceMemorySize, uResult));
		EngFreeMem(pSurfaceDescriptor);
		return uResult;
	}
	// Create a surface.

	dhsurf = (DHSURF) pSurfaceDescriptor;

	if (bSurface)
		hsurf = EngCreateDeviceSurface(dhsurf, sizl, ulBitmapType);
	else
		hsurf = (HSURF) EngCreateDeviceBitmap(dhsurf, sizl, ulBitmapType);

	if (!hsurf) {
		RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngCreateDevice*() failed\n");
		FreeSurfaceDescriptor(pSurfaceDescriptor);
		return STATUS_INVALID_HANDLE;
	}

	if (!EngModifySurface(hsurf, hdev, ulHooks, 0, (DHSURF) dhsurf, pSurfaceDescriptor->pSurfaceData, uStride, NULL)) {
		RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngModifySurface() failed\n");
		EngDeleteSurface(hsurf);
		FreeSurfaceDescriptor(pSurfaceDescriptor);
		return STATUS_INVALID_HANDLE;
	}

	pSurfaceDescriptor->cx = sizl.cx;
	pSurfaceDescriptor->cy = sizl.cy;
	pSurfaceDescriptor->lDelta = uStride;
	pSurfaceDescriptor->ulBitCount = ulBitCount;
/*
	memcpy(&pSurfaceDescriptor->BitmapHeader, &BitmapHeader, sizeof(BitmapHeader));
	pSurfaceDescriptor->BitmapHeader.FileHeader.bfSize = sizl.cx * sizl.cy * 4 + sizeof(BitmapHeader);
	pSurfaceDescriptor->BitmapHeader.V5Header.bV5Width = sizl.cx;
	pSurfaceDescriptor->BitmapHeader.V5Header.bV5Height = -(LONG) sizl.cy;
	pSurfaceDescriptor->BitmapHeader.V5Header.bV5SizeImage = sizl.cx * sizl.cy * 4;
*/
	if (sizl.cx > 50) {
//              DISPDBG((0, "Surface bitmap header %dx%d at %p (0x%x bytes), data at %p (0x%x bytes), pfns: %d\n", sizl.cx, sizl.cy,
//                       &pSurfaceDescriptor->BitmapHeader, sizeof(BitmapHeader), pSurfaceDescriptor->pSurfaceData, uSurfaceMemorySize,
//                       pSurfaceDescriptor->PfnArray.uNumberOf4kPages));
		DISPDBG((0, "Surface %dx%d, data at %p (0x%x bytes), pfns: %d\n", sizl.cx, sizl.cy, pSurfaceDescriptor->pSurfaceData, uSurfaceMemorySize,
			 pSurfaceDescriptor->PfnArray.uNumberOf4kPages));
		DISPDBG((0, "First phys pages: 0x%x, 0x%x, 0x%x\n", pSurfaceDescriptor->PfnArray.Pfn[0], pSurfaceDescriptor->PfnArray.Pfn[1],
			 pSurfaceDescriptor->PfnArray.Pfn[2]));
	}

	*ppSurfaceDescriptor = pSurfaceDescriptor;
	*pHsurf = hsurf;

	return 0;
}

/******************************Public*Routine******************************\
* DrvEnableSurface
*
* Enable the surface for the device. Hook the calls this driver supports.
*
* Return: Handle to the surface if successful, 0 for failure.
*
\**************************************************************************/

HSURF APIENTRY DrvEnableSurface(
	DHPDEV dhpdev
)
{
	PPDEV ppdev;
	HSURF hsurf;
	SIZEL sizl;
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
	LONG Status;

	// Create engine bitmap around frame buffer.

	DISPDBG((0, "DrvEnableSurface\n"));

	ppdev = (PPDEV) dhpdev;

	sizl.cx = ppdev->cxScreen;
	sizl.cy = ppdev->cyScreen;

	Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(TRUE, ppdev->hdevEng, ppdev->ulBitCount, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);
	if (Status < 0) {
		DISPDBG((0, "DrvEnableSurface(): AllocateNonOpaqueDeviceSurfaceOrBitmap() failed, status %d\n", Status));
		return NULL;
	}

	ppdev->hsurfEng = (HSURF) hsurf;
	ppdev->pScreenSurfaceDescriptor = (PVOID) pSurfaceDescriptor;

	pSurfaceDescriptor->bIsScreen = TRUE;

	return hsurf;
}

/******************************Public*Routine******************************\
* DrvDisableSurface
*
* Free resources allocated by DrvEnableSurface. Release the surface.
*
\**************************************************************************/

VOID APIENTRY DrvDisableSurface(
	DHPDEV dhpdev
)
{
	PPDEV ppdev = (PPDEV) dhpdev;

	DISPDBG((0, "DrvDisableSurface\n"));

	EngDeleteSurface(ppdev->hsurfEng);

	// deallocate SURFACE_DESCRIPTOR structure.

	FreeSurfaceDescriptor(ppdev->pScreenSurfaceDescriptor);
	ppdev->pScreenSurfaceDescriptor = NULL;
}

HBITMAP APIENTRY DrvCreateDeviceBitmap(
	IN DHPDEV dhpdev,
	IN SIZEL sizl,
	IN ULONG iFormat
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
	ULONG ulBitCount;
	HSURF hsurf;
	LONG Status;

	PPDEV ppdev = (PPDEV) dhpdev;

// DISPDBG((1,"CreateDeviceBitmap: %dx%d\n", sizl.cx, sizl.cy));

	switch (iFormat) {
	case BMF_8BPP:
		ulBitCount = 8;
		break;
	case BMF_16BPP:
		ulBitCount = 16;
		break;
	case BMF_24BPP:
		ulBitCount = 24;
		break;
	case BMF_32BPP:
		ulBitCount = 32;
		break;
	default:
		return NULL;
	}

	Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(FALSE, ppdev->hdevEng, ulBitCount, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);
	if (Status < 0) {
		DISPDBG((0, "DrvCreateDeviceBitmap(): AllocateNonOpaqueDeviceSurfaceOrBitmap() failed, status %d\n", Status));
		return NULL;
	}

	pSurfaceDescriptor->bIsScreen = FALSE;

	return (HBITMAP) hsurf;
}

VOID APIENTRY DrvDeleteDeviceBitmap(
	IN DHSURF dhsurf
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor;

// DISPDBG((1, "DeleteDeviceBitmap:\n"));

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) dhsurf;

	FreeSurfaceDescriptor(pSurfaceDescriptor);
}

BOOL APIENTRY DrvAssertMode(
	DHPDEV dhpdev,
	BOOL bEnable
)
{
	PPDEV ppdev = (PPDEV) dhpdev;

	UNREFERENCED_PARAMETER(bEnable);
	UNREFERENCED_PARAMETER(ppdev);

	DISPDBG((0, "DrvAssertMode(%lx, %lx)\n", dhpdev, bEnable));

	return TRUE;

}

ULONG SupportVideoMode(
	ULONG cjIn,
	PQV_SUPPORT_MODE pQvSupportMode
)
{
	if (cjIn < sizeof(QV_SUPPORT_MODE) || !pQvSupportMode)
		return QV_INVALID_PARAMETER;

	if (!IS_RESOLUTION_VALID(pQvSupportMode->uWidth, pQvSupportMode->uHeight))
		return QV_SUPPORT_MODE_INVALID_RESOLUTION;

	if (pQvSupportMode->uBpp != 16 && pQvSupportMode->uBpp != 24 && pQvSupportMode->uBpp != 32)
		return QV_SUPPORT_MODE_INVALID_BPP;

	g_uWidth = pQvSupportMode->uWidth;
	g_uHeight = pQvSupportMode->uHeight;
	g_uBpp = pQvSupportMode->uBpp;

	return QV_SUCCESS;
}

ULONG GetPfnList(
	SURFOBJ * pso,
	ULONG cjIn,
	PQV_GET_PFN_LIST pQvGetPfnList,
	ULONG cjOut,
	PQV_GET_PFN_LIST_RESPONSE pQvGetPfnListResponse
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
	DWORD dwResult;
	QVMINI_GET_PFN_LIST QvminiGetPfnList;
	ULONG nbReturned;

	if (cjIn < sizeof(QV_GET_PFN_LIST) || !pQvGetPfnList || cjOut < sizeof(QV_GET_PFN_LIST_RESPONSE) || !pQvGetPfnListResponse)
		return QV_INVALID_PARAMETER;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pso->dhsurf;
	if (!pSurfaceDescriptor || !pSurfaceDescriptor->ppdev)
		// A surface is managed by GDI
		return QV_INVALID_PARAMETER;

	QvminiGetPfnList.pVirtualAddress = pQvGetPfnList->pVirtualAddress;
	QvminiGetPfnList.uRegionSize = pQvGetPfnList->uRegionSize;

	dwResult = EngDeviceIoControl(pSurfaceDescriptor->ppdev->hDriver,
				      IOCTL_QVMINI_GET_PFN_LIST, &QvminiGetPfnList, sizeof(QvminiGetPfnList), &pQvGetPfnListResponse->PfnArray,
				      sizeof(PFN_ARRAY), &nbReturned);

	if (0 != dwResult) {
		DISPDBG((0, "GetPfnList(): EngDeviceIoControl(IOCTL_QVMINI_GET_PFN_LIST) failed with error %d\n", dwResult));
		return QV_INVALID_PARAMETER;
	}

	pQvGetPfnListResponse->uMagic = QVIDEO_MAGIC;

	return QV_SUCCESS;
}

ULONG GetSurfaceData(
	SURFOBJ * pso,
	ULONG cjOut,
	PQV_GET_SURFACE_DATA_RESPONSE pQvGetSurfaceDataResponse
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
//      PSURFACE_DESCRIPTOR pScreenSurfaceDescriptor = NULL;
//      ULONG uResult;
//      ULONG uSurfaceMemorySize;

	if (!pso || cjOut < sizeof(QV_GET_SURFACE_DATA_RESPONSE) || !pQvGetSurfaceDataResponse)
		return QV_INVALID_PARAMETER;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pso->dhsurf;
	if (!pSurfaceDescriptor || !pSurfaceDescriptor->ppdev)
		// A surface is managed by GDI
		return QV_INVALID_PARAMETER;

/*
	if (!pSurfaceDescriptor->bIsScreen) {

		if (pSurfaceDescriptor->pSurfaceData)
			FreeSurfaceData(pSurfaceDescriptor);

		pScreenSurfaceDescriptor = pSurfaceDescriptor->ppdev->pScreenSurfaceDescriptor;
		if (!pScreenSurfaceDescriptor) {
			DISPDBG((0, "GetSurfaceData(): pScreenSurfaceDescriptor is NULL\n"));
			return QV_INVALID_PARAMETER;
		}

		// FIXME: don't know why, but every 2nd line is some crap, so treat this as
		// twice long signle line 
		uSurfaceMemorySize = (ULONG) (2 * pScreenSurfaceDescriptor->lDelta * pScreenSurfaceDescriptor->cy);

		uResult = AllocateSurfaceData(pSurfaceDescriptor, uSurfaceMemorySize);
		if (0 != uResult) {
			DISPDBG((0, "GetSurfaceData(%p): AllocateSurfaceData(%d) failed with error %d\n", pso, uSurfaceMemorySize, uResult));
			return uResult;
		}

		DISPDBG((0, "GetSurfaceData(%p): New surface data at %p, size %d\n", pso, uSurfaceMemorySize));
	}
*/
	pQvGetSurfaceDataResponse->uMagic = QVIDEO_MAGIC;

	pQvGetSurfaceDataResponse->cx = pSurfaceDescriptor->cx;
	pQvGetSurfaceDataResponse->cy = pSurfaceDescriptor->cy;
	pQvGetSurfaceDataResponse->lDelta = pSurfaceDescriptor->lDelta;
	pQvGetSurfaceDataResponse->ulBitCount = pSurfaceDescriptor->ulBitCount;
	pQvGetSurfaceDataResponse->bIsScreen = pSurfaceDescriptor->bIsScreen;

	memcpy(&pQvGetSurfaceDataResponse->PfnArray, &pSurfaceDescriptor->PfnArray, sizeof(PFN_ARRAY));

	return QV_SUCCESS;
}

BOOL CALLBACK UnmapDamageNotificationEventProc(
	DRIVEROBJ * pDriverObj
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pDriverObj->pvObj;
	DISPDBG((0, "UnmapDamageNotificationEventProc(): unmapping 0x%p\n", pSurfaceDescriptor->pDamageNotificationEvent));

	EngUnmapEvent(pSurfaceDescriptor->pDamageNotificationEvent);

	pSurfaceDescriptor->pDamageNotificationEvent = NULL;
	pSurfaceDescriptor->hDriverObj = NULL;

	return TRUE;
}

ULONG WatchSurface(
	SURFOBJ * pso,
	ULONG cjIn,
	PQV_WATCH_SURFACE pQvWatchSurface
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
	PEVENT pDamageNotificationEvent = NULL;

	if (!pso || cjIn < sizeof(QV_WATCH_SURFACE) || !pQvWatchSurface)
		return QV_INVALID_PARAMETER;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pso->dhsurf;
	if (!pSurfaceDescriptor)
		// A surface is managed by GDI
		return QV_INVALID_PARAMETER;

	DISPDBG((0, "WatchSurface(%p): hEvent 0x%x\n", pso, pQvWatchSurface->hUserModeEvent));

	if (pSurfaceDescriptor->hDriverObj) {
		DISPDBG((0, "WatchSurface(%p): Surface is already watched\n", pso));
		return QV_INVALID_PARAMETER;
	}

	pDamageNotificationEvent = EngMapEvent(pso->hdev, pQvWatchSurface->hUserModeEvent, NULL, NULL, NULL);
	if (!pDamageNotificationEvent) {
		DISPDBG((0, "WatchSurface(): EngMapEvent(0x%x) failed, error %d\n", pQvWatchSurface->hUserModeEvent, EngGetLastError()));
		return QV_INVALID_HANDLE;
	}

	DISPDBG((0, "WatchSurface(): pEvent 0x%p\n", pDamageNotificationEvent));

	// *Maybe* this surface can be accessed by UnmapDamageNotificationEventProc() or DrvSynchronizeSurface() right after EngCreateDriverObj(),
	// so pSurfaceDescriptor->pDamageNotificationEvent must be set before calling it.
	pSurfaceDescriptor->pDamageNotificationEvent = pDamageNotificationEvent;

	// Install a notification callback for process deletion. 
	// We have to unmap the event if the client process terminates unexpectedly.
	pSurfaceDescriptor->hDriverObj = EngCreateDriverObj(pSurfaceDescriptor, UnmapDamageNotificationEventProc, pso->hdev);
	if (!pSurfaceDescriptor->hDriverObj) {
		DISPDBG((0, "WatchSurface(): EngCreateDriverObj(0x%p) failed, error %d\n", pSurfaceDescriptor, EngGetLastError()));

		pSurfaceDescriptor->pDamageNotificationEvent = NULL;
		EngUnmapEvent(pDamageNotificationEvent);
		return QV_INVALID_HANDLE;
	}

	DISPDBG((0, "WatchSurface(): hDriverObj 0x%p\n", pSurfaceDescriptor->hDriverObj));

	return QV_SUCCESS;
}

ULONG StopWatchingSurface(
	SURFOBJ * pso
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;

	if (!pso)
		return QV_INVALID_PARAMETER;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pso->dhsurf;
	if (!pSurfaceDescriptor)
		// A surface is managed by GDI
		return QV_INVALID_PARAMETER;

	if (!pSurfaceDescriptor->hDriverObj) {
		DISPDBG((0, "StopWatchingSurface(): hDriverObj is zero\n"));
		return QV_INVALID_PARAMETER;
	}

	DISPDBG((0, "StopWatchingSurface(%p)\n", pso));

	// Require a cleanup callback to be called.
	// pSurfaceDescriptor->hDriverObj will be set no NULL by the callback.
	EngDeleteDriverObj(pSurfaceDescriptor->hDriverObj, TRUE, FALSE);

	return QV_SUCCESS;
}

ULONG APIENTRY DrvEscape(
	SURFOBJ * pso,
	ULONG iEsc,
	ULONG cjIn,
	PVOID pvIn,
	ULONG cjOut,
	PVOID pvOut
)
{
	if ((cjIn < sizeof(ULONG)) || !pvIn || (*(PULONG) pvIn != QVIDEO_MAGIC))
		// 0 means "not supported"
		return 0;

	switch (iEsc) {
	case QVESC_SUPPORT_MODE:
		return SupportVideoMode(cjIn, pvIn);

	case QVESC_GET_SURFACE_DATA:
		return GetSurfaceData(pso, cjOut, pvOut);

	case QVESC_WATCH_SURFACE:
		return WatchSurface(pso, cjIn, pvIn);

	case QVESC_STOP_WATCHING_SURFACE:
		return StopWatchingSurface(pso);

	case QVESC_GET_PFN_LIST:
		return GetPfnList(pso, cjIn, pvIn, cjOut, pvOut);

	default:
		// 0 means "not supported"
		return 0;
	}
}

VOID APIENTRY DrvSynchronizeSurface(
	SURFOBJ * pso,
	RECTL * prcl,
	FLONG fl
)
{
	PSURFACE_DESCRIPTOR pSurfaceDescriptor = NULL;
	UNREFERENCED_PARAMETER(prcl);
	UNREFERENCED_PARAMETER(fl);

	if (!pso)
		return;

	pSurfaceDescriptor = (PSURFACE_DESCRIPTOR) pso->dhsurf;
	if (!pSurfaceDescriptor)
		// A surface is managed by GDI
		return;

	if (!pSurfaceDescriptor->pDamageNotificationEvent)
		// This surface is not watched.
		return;

	EngSetEvent(pSurfaceDescriptor->pDamageNotificationEvent);
#if DBG
	if (!(InterlockedIncrement(&g_lCounter) % 10)) {
		if (prcl)
			DISPDBG((0, "DrvSynchronizeSurface(%p), %dx%d - %dx%d, fl: %d, %d\n", pso, prcl->left, prcl->top, prcl->right, prcl->bottom, fl,
				 g_lCounter));
		else
			DISPDBG((0, "DrvSynchronizeSurface(%p), fl: %d, %d, iuniq %d\n", pso, fl, g_lCounter, pso->iUniq));
	}
#endif
}
