#include "driver.h"

#define SYSTM_LOGFONT {16,7,0,0,700,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,VARIABLE_PITCH | FF_DONTCARE,L"System"}
#define HELVE_LOGFONT {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS,PROOF_QUALITY,VARIABLE_PITCH | FF_DONTCARE,L"MS Sans Serif"}
#define COURI_LOGFONT {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS,PROOF_QUALITY,FIXED_PITCH | FF_DONTCARE, L"Courier"}

// This is the basic devinfo for a default driver. This is used as a base and customized based
// on information passed back from the miniport driver.

const DEVINFO gDevInfoFrameBuffer = {
    GCAPS_OPAQUERECT,
    SYSTM_LOGFONT,		/* Default font description */
    HELVE_LOGFONT,		/* ANSI variable font description */
    COURI_LOGFONT,		/* ANSI fixed font description */
    0,			/* Count of device fonts */
    0,			/* Preferred DIB format */
    8,			/* Width of color dither */
    8,			/* Height of color dither */
    0,			/* Default palette to use for this device */
    /*GCAPS2_SYNCTIMER | */ GCAPS2_SYNCFLUSH
};

/******************************Public*Routine******************************\
* bInitPDEV
*
* Determine the mode we should be in based on the DEVMODE passed in.
* For virtual devices we don't bother querying the miniport.
*
\**************************************************************************/

BOOL InitPdev(
    PDEV *ppdev,
    DEVMODEW *pDevMode,
    GDIINFO *pGdiInfo,
    DEVINFO *pDevInfo
    )
{
    //
    // Fill in the GDIINFO data structure with the information returned from
    // the kernel driver.
    //

    ppdev->ScreenWidth = pDevMode->dmPelsWidth;
    ppdev->ScreenHeight = pDevMode->dmPelsHeight;
    ppdev->BitsPerPel = pDevMode->dmBitsPerPel;
    ppdev->ScreenDelta = ppdev->ScreenWidth * ppdev->BitsPerPel;

    pGdiInfo->ulVersion = GDI_DRIVER_VERSION;
    pGdiInfo->ulTechnology = DT_RASDISPLAY;
    pGdiInfo->ulHorzSize = 320;
    pGdiInfo->ulVertSize = 240;

    pGdiInfo->ulHorzRes = ppdev->ScreenWidth;
    pGdiInfo->ulVertRes = ppdev->ScreenHeight;
    pGdiInfo->ulPanningHorzRes = 0;
    pGdiInfo->ulPanningVertRes = 0;
    pGdiInfo->cBitsPixel = 32;
    pGdiInfo->cPlanes = 1;
    pGdiInfo->ulVRefresh = 1;	// not used
    pGdiInfo->ulBltAlignment = 1;	// We don't have accelerated screen-to-screen blts, and any window alignment is okay

    pGdiInfo->ulLogPixelsX = pDevMode->dmLogPixels;
    pGdiInfo->ulLogPixelsY = pDevMode->dmLogPixels;

    pGdiInfo->flTextCaps = TC_RA_ABLE;

    pGdiInfo->flRaster = 0;	// flRaster is reserved by DDI

    pGdiInfo->ulDACRed = 8;
    pGdiInfo->ulDACGreen = 8;
    pGdiInfo->ulDACBlue = 8;

    pGdiInfo->ulAspectX = 0x24;	// One-to-one aspect ratio
    pGdiInfo->ulAspectY = 0x24;
    pGdiInfo->ulAspectXY = 0x33;

    pGdiInfo->xStyleStep = 1;	// A style unit is 3 pels
    pGdiInfo->yStyleStep = 1;
    pGdiInfo->denStyleStep = 3;

    pGdiInfo->ptlPhysOffset.x = 0;
    pGdiInfo->ptlPhysOffset.y = 0;
    pGdiInfo->szlPhysSize.cx = 0;
    pGdiInfo->szlPhysSize.cy = 0;

    // RGB and CMY color info.

    pGdiInfo->ciDevice.Red.x = 6700;
    pGdiInfo->ciDevice.Red.y = 3300;
    pGdiInfo->ciDevice.Red.Y = 0;
    pGdiInfo->ciDevice.Green.x = 2100;
    pGdiInfo->ciDevice.Green.y = 7100;
    pGdiInfo->ciDevice.Green.Y = 0;
    pGdiInfo->ciDevice.Blue.x = 1400;
    pGdiInfo->ciDevice.Blue.y = 800;
    pGdiInfo->ciDevice.Blue.Y = 0;
    pGdiInfo->ciDevice.AlignmentWhite.x = 3127;
    pGdiInfo->ciDevice.AlignmentWhite.y = 3290;
    pGdiInfo->ciDevice.AlignmentWhite.Y = 0;

    pGdiInfo->ciDevice.RedGamma = 20000;
    pGdiInfo->ciDevice.GreenGamma = 20000;
    pGdiInfo->ciDevice.BlueGamma = 20000;

    pGdiInfo->ciDevice.Cyan.x = 0;
    pGdiInfo->ciDevice.Cyan.y = 0;
    pGdiInfo->ciDevice.Cyan.Y = 0;
    pGdiInfo->ciDevice.Magenta.x = 0;
    pGdiInfo->ciDevice.Magenta.y = 0;
    pGdiInfo->ciDevice.Magenta.Y = 0;
    pGdiInfo->ciDevice.Yellow.x = 0;
    pGdiInfo->ciDevice.Yellow.y = 0;
    pGdiInfo->ciDevice.Yellow.Y = 0;

    // No dye correction for raster displays.

    pGdiInfo->ciDevice.MagentaInCyanDye = 0;
    pGdiInfo->ciDevice.YellowInCyanDye = 0;
    pGdiInfo->ciDevice.CyanInMagentaDye = 0;
    pGdiInfo->ciDevice.YellowInMagentaDye = 0;
    pGdiInfo->ciDevice.CyanInYellowDye = 0;
    pGdiInfo->ciDevice.MagentaInYellowDye = 0;

    pGdiInfo->ulDevicePelsDPI = 0;	// For printers only
    pGdiInfo->ulPrimaryOrder = PRIMARY_ORDER_CBA;

    // Note: this should be modified later to take into account the size
    // of the display and the resolution.

    pGdiInfo->ulHTPatternSize = HT_PATSIZE_4x4_M;

    pGdiInfo->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;

    pGdiInfo->ulNumColors = (ULONG) -1;
    pGdiInfo->ulNumPalReg = 0;

    // Fill in the basic devinfo structure

    *pDevInfo = gDevInfoFrameBuffer;

    switch (ppdev->BitsPerPel)
    {
    case 16:
        pGdiInfo->ulHTOutputFormat = HT_FORMAT_16BPP;
        pDevInfo->iDitherFormat = BMF_16BPP;
        break;
    case 24:
        pGdiInfo->ulHTOutputFormat = HT_FORMAT_24BPP;
        pDevInfo->iDitherFormat = BMF_24BPP;
        break;
    case 32:
        pGdiInfo->ulHTOutputFormat = HT_FORMAT_32BPP;
        pDevInfo->iDitherFormat = BMF_32BPP;
        break;
    }

    pDevInfo->hpalDefault = ppdev->DefaultPalette = EngCreatePalette(PAL_BITFIELDS, 0, NULL, 0xFF0000, 0xFF00, 0xFF);
    if (!pDevInfo->hpalDefault)
        return FALSE;

    return TRUE;
}
