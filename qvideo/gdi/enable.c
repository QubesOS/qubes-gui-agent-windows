#include "driver.h"
#include "support.h"

#include <dpfilter.h>

#define STATUS_SUCCESS 0

#if DBG
// This is only to make linker happy. Enabling security checks
// causes linking a special library that expects that DriverEntry is present.
void DriverEntry()
{
    ERRORF("THIS SHOULD NOT BE CALLED, EVER");
}
#endif

// The driver function table with all function index/address pairs
static DRVFN g_DrvFunctions[] =
{
    { INDEX_DrvGetModes, (PFN) DrvGetModes },
    { INDEX_DrvSynchronizeSurface, (PFN) DrvSynchronizeSurface },
    { INDEX_DrvEnablePDEV, (PFN) DrvEnablePDEV },
    { INDEX_DrvCompletePDEV, (PFN) DrvCompletePDEV },
    { INDEX_DrvDisablePDEV, (PFN) DrvDisablePDEV },
    { INDEX_DrvEnableSurface, (PFN) DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN) DrvDisableSurface },
    { INDEX_DrvAssertMode, (PFN) DrvAssertMode },
    //{INDEX_DrvCreateDeviceBitmap, (PFN) DrvCreateDeviceBitmap},
    //{INDEX_DrvDeleteDeviceBitmap, (PFN) DrvDeleteDeviceBitmap},
    { INDEX_DrvEscape, (PFN) DrvEscape }
};

// default mode, before gui agent connects
ULONG g_uDefaultWidth = 800;
ULONG g_uDefaultHeight = 600;
// initial second mode, will be updated by gui agent based on dom0 mode
ULONG g_uWidth = 1280;
ULONG g_uHeight = 800;
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
    __in_bcount(cj) DRVENABLEDATA *pded
    )
{
    BOOL status = FALSE;
    FUNCTION_ENTER();

    if ((iEngineVersion < DDI_DRIVER_VERSION_NT5) || (cj < sizeof(DRVENABLEDATA)))
    {
        ERRORF("Unsupported engine version %d, DRVENABLEDATA size %d", iEngineVersion, cj);
        goto cleanup;
    }

    pded->pdrvfn = g_DrvFunctions;
    pded->c = sizeof(g_DrvFunctions) / sizeof(DRVFN);
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;

    ReadRegistryConfig();
    status = TRUE;

cleanup:
    FUNCTION_EXIT();
    return status;
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
    __in DEVMODEW *pDevmode,	// Pointer to DEVMODE
    __in_opt WCHAR *pwszLogAddress,	// Logical address
    __in ULONG cPatterns,	// number of patterns
    __in_opt HSURF *ahsurfPatterns,	// return standard patterns
    __in ULONG cjGdiInfo,	// Length of memory pointed to by pGdiInfo
    __out_bcount(cjGdiInfo) ULONG *pGdiInfo,	// Pointer to GdiInfo structure
    __in ULONG cjDevInfo,	// Length of following PDEVINFO structure
    __out_bcount(cjDevInfo) DEVINFO *pDevInfo,	// physical device information structure
    __in_opt HDEV hdev,	// HDEV, used for callbacks
    __in_opt WCHAR *pwszDeviceName,	// DeviceName - not used
    __in HANDLE hDriver	// Handle to base driver
    )
{
    PDEV *ppdev = NULL;

    FUNCTION_ENTER();

    UNREFERENCED_PARAMETER(pwszLogAddress);
    UNREFERENCED_PARAMETER(cPatterns);
    UNREFERENCED_PARAMETER(ahsurfPatterns);
    UNREFERENCED_PARAMETER(hdev);
    UNREFERENCED_PARAMETER(pwszDeviceName);

    if (sizeof(DEVINFO) > cjDevInfo)
    {
        ERRORF("insufficient pDevInfo memory");
        goto cleanup;
    }

    if (sizeof(GDIINFO) > cjGdiInfo)
    {
        ERRORF("insufficient pGdiInfo memory");
        goto cleanup;
    }

    // Allocate a physical device structure.
    ppdev = (PDEV *) EngAllocMem(FL_ZERO_MEMORY, sizeof(PDEV), ALLOC_TAG);
    if (ppdev == NULL)
    {
        ERRORF("EngAllocMem() failed");
        goto cleanup;
    }

    // Save the screen handle in the PDEV.
    ppdev->DriverHandle = hDriver;

    // Get the current screen mode information. Set up device caps and devinfo.
    if (!InitPdev(ppdev, pDevmode, (GDIINFO *) pGdiInfo, pDevInfo))
    {
        ERRORF("InitPdev() failed");
        EngFreeMem(ppdev);
        ppdev = NULL;
        goto cleanup;
    }

cleanup:

    FUNCTION_EXIT();
    return (DHPDEV)ppdev;
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
    FUNCTION_ENTER();
    ((PDEV *)dhpdev)->EngPdevHandle = hdev;
    FUNCTION_EXIT();
}

ULONG APIENTRY DrvGetModes(
    HANDLE hDriver,
    ULONG cjSize,
    DEVMODEW *pdm
    )
{
    ULONG ulBytesWritten = 0, ulBytesNeeded = 2 * sizeof(DEVMODEW);
    ULONG ulReturnValue;
    DWORD i;

    FUNCTION_ENTER();

    UNREFERENCED_PARAMETER(hDriver);
    UNREFERENCED_PARAMETER(cjSize);

    DEBUGF("pdm: %p, size: %lu, bytes needed: %lu", pdm, cjSize, ulBytesNeeded);
    if (pdm == NULL)
    {
        ulReturnValue = ulBytesNeeded;
    }
    else if (cjSize < ulBytesNeeded)
    {
        ulReturnValue = 0;
    }
    else
    {
        ulBytesWritten = ulBytesNeeded;

        RtlZeroMemory(pdm, ulBytesNeeded);
        for (i = 0; i < 2; i++)
        {
            memcpy(pdm[i].dmDeviceName, DLL_NAME, sizeof(DLL_NAME));

            pdm[i].dmSpecVersion = DM_SPECVERSION;
            pdm[i].dmDriverVersion = DM_SPECVERSION;

            pdm[i].dmDriverExtra = 0;
            pdm[i].dmSize = sizeof(DEVMODEW);
            pdm[i].dmBitsPerPel = g_uBpp;
            switch (i)
            {
            case 0:
                pdm[i].dmPelsWidth = g_uDefaultWidth;
                pdm[i].dmPelsHeight = g_uDefaultHeight;
                break;
            case 1:
                pdm[i].dmPelsWidth = g_uWidth;
                pdm[i].dmPelsHeight = g_uHeight;
                break;
            }
            pdm[i].dmDisplayFrequency = 75;

            pdm[i].dmDisplayFlags = 0;

            pdm[i].dmPanningWidth = pdm[i].dmPelsWidth;
            pdm[i].dmPanningHeight = pdm[i].dmPelsHeight;

            pdm[i].dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
        }

        ulReturnValue = ulBytesWritten;
    }

    DEBUGF("return %lu", ulReturnValue);

    FUNCTION_EXIT();
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
    PDEV *ppdev = (PDEV *) dhpdev;

    FUNCTION_ENTER();

    EngDeletePalette(ppdev->DefaultPalette);
    EngFreeMem(ppdev);
}

ULONG AllocateSurfaceMemory(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    QVMINI_ALLOCATE_MEMORY_RESPONSE *pQvminiAllocateMemoryResponse = NULL;
    QVMINI_ALLOCATE_MEMORY QvminiAllocateMemory;
    ULONG nbReturned;
    DWORD status = STATUS_NO_MEMORY;

    FUNCTION_ENTER();

    pQvminiAllocateMemoryResponse = (QVMINI_ALLOCATE_MEMORY_RESPONSE *)
        EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE), ALLOC_TAG);

    if (!pQvminiAllocateMemoryResponse)
    {
        ERRORF("EngAllocMem(QVMINI_ALLOCATE_MEMORY_RESPONSE) failed");
        goto cleanup;
    }

    QvminiAllocateMemory.Size = uLength;

    DEBUGF("calling IOCTL_QVMINI_ALLOCATE_MEMORY: size %lu", uLength);
    status = EngDeviceIoControl(
        pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_ALLOCATE_MEMORY,
        &QvminiAllocateMemory,
        sizeof(QvminiAllocateMemory),
        pQvminiAllocateMemoryResponse,
        sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE),
        &nbReturned);

    if (STATUS_SUCCESS != status)
    {
        ERRORF("EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_MEMORY) failed: 0x%lx", status);
        EngFreeMem(pQvminiAllocateMemoryResponse);
        goto cleanup;
    }

    pSurfaceDescriptor->SurfaceData = pQvminiAllocateMemoryResponse->VirtualAddress;
    pSurfaceDescriptor->pPfnArray = pQvminiAllocateMemoryResponse->PfnArray;

    EngFreeMem(pQvminiAllocateMemoryResponse);
    status = STATUS_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

ULONG AllocateSection(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    QVMINI_ALLOCATE_SECTION_RESPONSE *pQvminiAllocateSectionResponse = NULL;
    QVMINI_ALLOCATE_SECTION QvminiAllocateSection;
    ULONG nbReturned;
    DWORD status = STATUS_NO_MEMORY;

    FUNCTION_ENTER();

    pQvminiAllocateSectionResponse = (QVMINI_ALLOCATE_SECTION_RESPONSE *)
        EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE), ALLOC_TAG);

    if (!pQvminiAllocateSectionResponse)
    {
        ERRORF("EngAllocMem(QVMINI_ALLOCATE_SECTION_RESPONSE) failed");
        goto cleanup;
    }

    QvminiAllocateSection.Size = uLength;
    QvminiAllocateSection.UseDirtyBits = g_bUseDirtyBits;

    DEBUGF("calling IOCTL_QVMINI_ALLOCATE_SECTION: size %lu, use dirty bits: %d", uLength, g_bUseDirtyBits);
    status = EngDeviceIoControl(
        pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_ALLOCATE_SECTION,
        &QvminiAllocateSection,
        sizeof(QvminiAllocateSection),
        pQvminiAllocateSectionResponse,
        sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE),
        &nbReturned);

    if (STATUS_SUCCESS != status)
    {
        ERRORF("EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_SECTION) failed: 0x%lx", status);
        EngFreeMem(pQvminiAllocateSectionResponse);
        goto cleanup;
    }

    pSurfaceDescriptor->SurfaceData = pQvminiAllocateSectionResponse->VirtualAddress;
    pSurfaceDescriptor->SurfaceSection = pQvminiAllocateSectionResponse->Section;
    pSurfaceDescriptor->SectionObject = pQvminiAllocateSectionResponse->SectionObject;
    pSurfaceDescriptor->Mdl = pQvminiAllocateSectionResponse->Mdl;

    pSurfaceDescriptor->DirtySectionObject = pQvminiAllocateSectionResponse->DirtySectionObject;
    pSurfaceDescriptor->DirtySection = pQvminiAllocateSectionResponse->DirtySection;
    pSurfaceDescriptor->DirtyPages = pQvminiAllocateSectionResponse->DirtyPages;

    pSurfaceDescriptor->pPfnArray = pQvminiAllocateSectionResponse->PfnArray;

    EngFreeMem(pQvminiAllocateSectionResponse);
    status = STATUS_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

ULONG AllocateSurfaceData(
    BOOLEAN bSurface,
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    ULONG status;

    FUNCTION_ENTER();

    if (bSurface)
        status = AllocateSection(pSurfaceDescriptor, uLength);
    else
        status = AllocateSurfaceMemory(pSurfaceDescriptor, uLength);

    FUNCTION_EXIT();
    return status;
}

VOID FreeSurfaceMemory(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    DWORD dwResult;
    QVMINI_FREE_MEMORY QvminiFreeMemory;
    ULONG nbReturned;

    FUNCTION_ENTER();

    DEBUGF("surface: %p, data: %p", pSurfaceDescriptor, pSurfaceDescriptor->SurfaceData);

    QvminiFreeMemory.VirtualAddress = pSurfaceDescriptor->SurfaceData;
    QvminiFreeMemory.PfnArray = pSurfaceDescriptor->pPfnArray;

    dwResult = EngDeviceIoControl(pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_FREE_MEMORY, &QvminiFreeMemory, sizeof(QvminiFreeMemory), NULL, 0, &nbReturned);

    if (STATUS_SUCCESS != dwResult)
    {
        ERRORF("EngDeviceIoControl(IOCTL_QVMINI_FREE_MEMORY) failed: 0x%lx", dwResult);
    }

    FUNCTION_EXIT();
}

VOID FreeSection(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    DWORD dwResult;
    QVMINI_FREE_SECTION QvminiFreeSection;
    ULONG nbReturned;

    FUNCTION_ENTER();

    DEBUGF("surface: %p, data: %p", pSurfaceDescriptor, pSurfaceDescriptor->SurfaceData);

    QvminiFreeSection.VirtualAddress = pSurfaceDescriptor->SurfaceData;
    QvminiFreeSection.Section = pSurfaceDescriptor->SurfaceSection;
    QvminiFreeSection.SectionObject = pSurfaceDescriptor->SectionObject;
    QvminiFreeSection.Mdl = pSurfaceDescriptor->Mdl;

    QvminiFreeSection.DirtyPages = pSurfaceDescriptor->DirtyPages;
    QvminiFreeSection.DirtySection = pSurfaceDescriptor->DirtySection;
    QvminiFreeSection.DirtySectionObject = pSurfaceDescriptor->DirtySectionObject;

    QvminiFreeSection.PfnArray = pSurfaceDescriptor->pPfnArray;

    dwResult = EngDeviceIoControl(pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_FREE_SECTION, &QvminiFreeSection, sizeof(QvminiFreeSection), NULL, 0, &nbReturned);

    if (0 != dwResult)
    {
        ERRORF("EngDeviceIoControl(IOCTL_QVMINI_FREE_SECTION) failed: 0x%lx", dwResult);
    }

    FUNCTION_EXIT();
}

VOID FreeSurfaceData(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    FUNCTION_ENTER();

    if (pSurfaceDescriptor->IsScreen)
        FreeSection(pSurfaceDescriptor);
    else
        FreeSurfaceMemory(pSurfaceDescriptor);

    FUNCTION_EXIT();
}

VOID FreeSurfaceDescriptor(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    FUNCTION_ENTER();

    if (!pSurfaceDescriptor)
        goto cleanup;

    if (pSurfaceDescriptor->DriverObj)
    {
        // Require a cleanup callback to be called.
        // pSurfaceDescriptor->hDriverObj will be set no NULL by the callback.
        EngDeleteDriverObj(pSurfaceDescriptor->DriverObj, TRUE, FALSE);
    }

    FreeSurfaceData(pSurfaceDescriptor);
    EngFreeMem(pSurfaceDescriptor);

cleanup:
    FUNCTION_EXIT();
}

ULONG AllocateNonOpaqueDeviceSurfaceOrBitmap(
    BOOLEAN bSurface,
    HDEV hdev,
    ULONG ulBitCount,
    SIZEL sizl,
    ULONG ulHooks,
    HSURF *pHsurf,
    SURFACE_DESCRIPTOR **ppSurfaceDescriptor,
    PDEV *ppdev
    )
{
    ULONG ulBitmapType;
    ULONG uSurfaceMemorySize;
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;
    ULONG uStride;
    HSURF hsurf;
    DHSURF dhsurf;
    ULONG status = STATUS_INVALID_PARAMETER;

    FUNCTION_ENTER();

    if (!hdev || !pHsurf || !ppSurfaceDescriptor)
        goto cleanup;

    switch (ulBitCount)
    {
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
        goto cleanup;
    }

    uStride = (ulBitCount >> 3) * sizl.cx;

    uSurfaceMemorySize = (ULONG) (uStride * sizl.cy);

    DEBUGF("allocating surface data: %d x %d @ %lu, size %lu", sizl.cx, sizl.cy, ulBitCount, uSurfaceMemorySize);

    status = STATUS_NO_MEMORY;
    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) EngAllocMem(FL_ZERO_MEMORY, sizeof(SURFACE_DESCRIPTOR), ALLOC_TAG);
    if (!pSurfaceDescriptor)
    {
        ERRORF("EngAllocMem(SURFACE_DESCRIPTOR) failed");
        goto cleanup;
    }

    pSurfaceDescriptor->Pdev = ppdev;

    // pSurfaceDescriptor->ppdev must be set before calling this,
    // because we query our miniport through EngDeviceIoControl().
    status = AllocateSurfaceData(bSurface, pSurfaceDescriptor, uSurfaceMemorySize);
    if (STATUS_SUCCESS != status)
    {
        ERRORF("AllocateSurfaceData() failed: 0x%lx", status);
        EngFreeMem(pSurfaceDescriptor);
        goto cleanup;
    }

    // Create a surface.
    dhsurf = (DHSURF) pSurfaceDescriptor;

    if (bSurface)
        hsurf = EngCreateDeviceSurface(dhsurf, sizl, ulBitmapType);
    else
        hsurf = (HSURF) EngCreateDeviceBitmap(dhsurf, sizl, ulBitmapType);

    status = STATUS_INVALID_HANDLE;
    if (!hsurf)
    {
        ERRORF("EngCreateDevice*() failed");
        FreeSurfaceDescriptor(pSurfaceDescriptor);
        goto cleanup;
    }

    if (!EngModifySurface(hsurf, hdev, ulHooks, 0, (DHSURF) dhsurf, pSurfaceDescriptor->SurfaceData, uStride, NULL))
    {
        ERRORF("EngModifySurface() failed");
        EngDeleteSurface(hsurf);
        FreeSurfaceDescriptor(pSurfaceDescriptor);
        goto cleanup;
    }

    pSurfaceDescriptor->Width = sizl.cx;
    pSurfaceDescriptor->Height = sizl.cy;
    pSurfaceDescriptor->Delta = uStride;
    pSurfaceDescriptor->BitCount = ulBitCount;

    if (sizl.cx > 50)
    {
        DEBUGF("Surface %dx%d, data at %p (0x%x bytes), pfns: %d",
            sizl.cx, sizl.cy, pSurfaceDescriptor->SurfaceData, uSurfaceMemorySize,
            pSurfaceDescriptor->pPfnArray->NumberOf4kPages);
    }

    *ppSurfaceDescriptor = pSurfaceDescriptor;
    *pHsurf = hsurf;
    status = STATUS_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
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
    PDEV *ppdev;
    HSURF hsurf;
    SIZEL sizl;
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    LONG Status;

    FUNCTION_ENTER();

    // Create engine bitmap around frame buffer.
    ppdev = (PDEV *)dhpdev;

    sizl.cx = ppdev->ScreenWidth;
    sizl.cy = ppdev->ScreenHeight;

    Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(
        TRUE, ppdev->EngPdevHandle, ppdev->BitsPerPel, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);

    if (Status < 0)
    {
        ERRORF("AllocateNonOpaqueDeviceSurfaceOrBitmap() failed: 0x%lx", Status);
        return NULL;
    }

    ppdev->EngSurfaceHandle = (HSURF) hsurf;
    ppdev->ScreenSurfaceDescriptor = (PVOID) pSurfaceDescriptor;

    pSurfaceDescriptor->IsScreen = TRUE;

    FUNCTION_EXIT();
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
    PDEV *ppdev = (PDEV *) dhpdev;

    FUNCTION_ENTER();

    EngDeleteSurface(ppdev->EngSurfaceHandle);

    // deallocate SURFACE_DESCRIPTOR structure.

    FreeSurfaceDescriptor(ppdev->ScreenSurfaceDescriptor);
    ppdev->ScreenSurfaceDescriptor = NULL;
}

HBITMAP APIENTRY DrvCreateDeviceBitmap(
    IN DHPDEV dhpdev,
    IN SIZEL sizl,
    IN ULONG iFormat
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG ulBitCount;
    HSURF hsurf = NULL;
    LONG Status;

    PDEV *ppdev = (PDEV *) dhpdev;

    FUNCTION_ENTER();
    //DISPDBG((1,"CreateDeviceBitmap: %dx%d\n", sizl.cx, sizl.cy));

    switch (iFormat)
    {
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
        goto cleanup;
    }

    Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(
        FALSE, ppdev->EngPdevHandle, ulBitCount, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);

    if (Status != STATUS_SUCCESS)
    {
        ERRORF("AllocateNonOpaqueDeviceSurfaceOrBitmap() failed: 0x%lx", Status);
        goto cleanup;
    }

    pSurfaceDescriptor->IsScreen = FALSE;

cleanup:
    FUNCTION_EXIT();
    return (HBITMAP) hsurf;
}

VOID APIENTRY DrvDeleteDeviceBitmap(
    IN DHSURF dhsurf
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;

    FUNCTION_ENTER();

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) dhsurf;
    FreeSurfaceDescriptor(pSurfaceDescriptor);

    FUNCTION_EXIT();
}

BOOL APIENTRY DrvAssertMode(
    DHPDEV dhpdev,
    BOOL bEnable
    )
{
    PDEV *ppdev = (PDEV *) dhpdev;

    FUNCTION_ENTER();

    UNREFERENCED_PARAMETER(bEnable);
    UNREFERENCED_PARAMETER(ppdev);

    DEBUGF("DrvAssertMode(%lx, %lx)", dhpdev, bEnable);

    FUNCTION_EXIT();
    return TRUE;
}

ULONG UserSupportVideoMode(
    ULONG cjIn,
    QV_SUPPORT_MODE *pQvSupportMode
    )
{
    ULONG status = QV_INVALID_PARAMETER;

    FUNCTION_ENTER();

    if (cjIn < sizeof(QV_SUPPORT_MODE) || !pQvSupportMode)
        goto cleanup;

    status = QV_SUPPORT_MODE_INVALID_RESOLUTION;
    if (!IS_RESOLUTION_VALID(pQvSupportMode->Width, pQvSupportMode->Height))
        goto cleanup;

    status = QV_SUPPORT_MODE_INVALID_BPP;
    if (pQvSupportMode->Bpp != 16 && pQvSupportMode->Bpp != 24 && pQvSupportMode->Bpp != 32)
        goto cleanup;

    DEBUGF("SupportVideoMode(%ld x %ld @ %d)", pQvSupportMode->Width, pQvSupportMode->Height, pQvSupportMode->Bpp);
    g_uWidth = pQvSupportMode->Width;
    g_uHeight = pQvSupportMode->Height;
    g_uBpp = pQvSupportMode->Bpp;
    status = QV_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

ULONG UserGetSurfaceData(
    SURFOBJ *pso,
    ULONG cjIn,
    QV_GET_SURFACE_DATA *pQvGetSurfaceData,
    ULONG cjOut,
    QV_GET_SURFACE_DATA_RESPONSE *pQvGetSurfaceDataResponse
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG status = QV_INVALID_PARAMETER;

    FUNCTION_ENTER();

    if (!pso || cjOut < sizeof(QV_GET_SURFACE_DATA_RESPONSE) || !pQvGetSurfaceDataResponse
        || cjIn < sizeof(QV_GET_SURFACE_DATA) || !pQvGetSurfaceData)
        goto cleanup;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor || !pSurfaceDescriptor->Pdev)
    {
        // A surface is managed by GDI
        goto cleanup;
    }

    pQvGetSurfaceDataResponse->Magic = QVIDEO_MAGIC;

    pQvGetSurfaceDataResponse->Width = pSurfaceDescriptor->Width;
    pQvGetSurfaceDataResponse->Height = pSurfaceDescriptor->Height;
    pQvGetSurfaceDataResponse->Delta = pSurfaceDescriptor->Delta;
    pQvGetSurfaceDataResponse->Bpp = pSurfaceDescriptor->BitCount;
    pQvGetSurfaceDataResponse->IsScreen = pSurfaceDescriptor->IsScreen;

    DEBUGF("memcpy(%p, size %lu)", pQvGetSurfaceData->PfnArray, PFN_ARRAY_SIZE(pSurfaceDescriptor->Width, pSurfaceDescriptor->Height));
    memcpy(pQvGetSurfaceData->PfnArray, pSurfaceDescriptor->pPfnArray,
        PFN_ARRAY_SIZE(pSurfaceDescriptor->Width, pSurfaceDescriptor->Height));

    status = QV_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

BOOL CALLBACK UnmapDamageNotificationEventProc(
    DRIVEROBJ *pDriverObj
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;

    FUNCTION_ENTER();

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *)pDriverObj->pvObj;
    DEBUGF("unmapping %p", pSurfaceDescriptor->DamageNotificationEvent);

    EngUnmapEvent(pSurfaceDescriptor->DamageNotificationEvent);

    pSurfaceDescriptor->DamageNotificationEvent = NULL;
    pSurfaceDescriptor->DriverObj = NULL;

    FUNCTION_EXIT();
    return TRUE;
}

ULONG UserWatchSurface(
    SURFOBJ *pso,
    ULONG cjIn,
    QV_WATCH_SURFACE *pQvWatchSurface
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    PEVENT pDamageNotificationEvent = NULL;
    ULONG status = QV_INVALID_PARAMETER;

    FUNCTION_ENTER();

    if (!pso || cjIn < sizeof(QV_WATCH_SURFACE) || !pQvWatchSurface)
        goto cleanup;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
    {
        // A surface is managed by GDI
        goto cleanup;
    }

    DEBUGF("surface: %p, event: %p", pso, pQvWatchSurface->UserModeEvent);

    if (pSurfaceDescriptor->DriverObj)
    {
        DEBUGF("Surface %p is already watched", pso);
        goto cleanup;
    }

    status = QV_INVALID_HANDLE;
    pDamageNotificationEvent = EngMapEvent(pso->hdev, pQvWatchSurface->UserModeEvent, NULL, NULL, NULL);
    if (!pDamageNotificationEvent)
    {
        ERRORF("EngMapEvent(%p) failed: 0x%lx", pQvWatchSurface->UserModeEvent, EngGetLastError());
        goto cleanup;
    }

    DEBUGF("event: %p", pDamageNotificationEvent);

    // *Maybe* this surface can be accessed by UnmapDamageNotificationEventProc() or DrvSynchronizeSurface() right after EngCreateDriverObj(),
    // so pSurfaceDescriptor->pDamageNotificationEvent must be set before calling it.
    pSurfaceDescriptor->DamageNotificationEvent = pDamageNotificationEvent;

    // Install a notification callback for process deletion.
    // We have to unmap the event if the client process terminates unexpectedly.
    pSurfaceDescriptor->DriverObj = EngCreateDriverObj(pSurfaceDescriptor, UnmapDamageNotificationEventProc, pso->hdev);
    if (!pSurfaceDescriptor->DriverObj)
    {
        ERRORF("EngCreateDriverObj(%p) failed: 0x%lx", pSurfaceDescriptor, EngGetLastError());

        pSurfaceDescriptor->DamageNotificationEvent = NULL;
        EngUnmapEvent(pDamageNotificationEvent);
        goto cleanup;
    }

    DEBUGF("DriverObj: %p", pSurfaceDescriptor->DriverObj);
    status = QV_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

ULONG UserStopWatchingSurface(
    SURFOBJ *pso
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG status = QV_INVALID_PARAMETER;

    FUNCTION_ENTER();

    if (!pso)
        goto cleanup;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
    {
        // A surface is managed by GDI
        goto cleanup;
    }

    if (!pSurfaceDescriptor->DriverObj)
    {
        WARNINGF("DriverObj is zero");
        goto cleanup;
    }

    DEBUGF("surface: %p", pso);

    // Require a cleanup callback to be called.
    // pSurfaceDescriptor->hDriverObj will be set to NULL by the callback.
    EngDeleteDriverObj(pSurfaceDescriptor->DriverObj, TRUE, FALSE);
    status = QV_SUCCESS;

cleanup:
    FUNCTION_EXIT();
    return status;
}

ULONG APIENTRY DrvEscape(
    SURFOBJ *pso,
    ULONG iEsc,
    ULONG cjIn,
    void *pvIn,
    ULONG cjOut,
    void *pvOut
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;

    FUNCTION_ENTER();

    DEBUGF("surface: %p, code: 0x%lx", pso, iEsc);
    if ((cjIn < sizeof(ULONG)) || !pvIn || (*(PULONG) pvIn != QVIDEO_MAGIC))
    {
        // 0 means "not supported"
        WARNINGF("bad size/magic");
        return 0;
    }

    // FIXME: validate user buffers!

    switch (iEsc)
    {
    case QVESC_SUPPORT_MODE:
        return UserSupportVideoMode(cjIn, pvIn);

    case QVESC_GET_SURFACE_DATA:
        return UserGetSurfaceData(pso, cjIn, pvIn, cjOut, pvOut);

    case QVESC_WATCH_SURFACE:
        return UserWatchSurface(pso, cjIn, pvIn);

    case QVESC_STOP_WATCHING_SURFACE:
        return UserStopWatchingSurface(pso);

    case QVESC_SYNCHRONIZE:
        if (!g_bUseDirtyBits)
            return 0;
        
        pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
        if (!pSurfaceDescriptor || !pSurfaceDescriptor->Pdev)
            return QV_INVALID_PARAMETER;

        InterlockedExchange(&pSurfaceDescriptor->DirtyPages->Ready, 1);
        TRACEF("gui agent synchronized");
        return QV_SUCCESS;

    default:
        WARNINGF("bad code 0x%lx", iEsc);
        // 0 means "not supported"
        return 0;
    }
}

VOID APIENTRY DrvSynchronizeSurface(
    SURFOBJ *pso,
    RECTL *prcl,
    FLONG fl
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG uSize, uDirty;

    FUNCTION_ENTER();

    UNREFERENCED_PARAMETER(prcl);
    UNREFERENCED_PARAMETER(fl);

    if (!pso)
        return;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
        return;

    if (!pSurfaceDescriptor->DamageNotificationEvent)
        // This surface is not watched.
        return;

    // surface buffer size
    uSize = pSurfaceDescriptor->Delta * pSurfaceDescriptor->Height;

    // UpdateDirtyBits returns 0 also if the check was too early after a previous one.
    // This just returns 1 if using dirty bits is disabled.
    uDirty = UpdateDirtyBits(pSurfaceDescriptor->SurfaceData, uSize, pSurfaceDescriptor->DirtyPages);

    if (uDirty > 0) // only signal the event if something changed
        EngSetEvent(pSurfaceDescriptor->DamageNotificationEvent);

    FUNCTION_EXIT();
}
