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
* InitPDEV
*
* Determine the mode we should be in based on the DEVMODE passed in.
* For virtual devices we don't bother querying the miniport.
*
\**************************************************************************/

BOOL InitPdev(
    QV_PDEV *Pdev,
    DEVMODEW *DevMode,
    GDIINFO *DevCaps,
    DEVINFO *DevInfo
    )
{
    //
    // Fill in the GDIINFO data structure with the information returned from
    // the kernel driver.
    //

    Pdev->ScreenWidth = DevMode->dmPelsWidth;
    Pdev->ScreenHeight = DevMode->dmPelsHeight;
    Pdev->BitsPerPel = DevMode->dmBitsPerPel;
    Pdev->ScreenDelta = Pdev->ScreenWidth * Pdev->BitsPerPel;

    DevCaps->ulVersion = GDI_DRIVER_VERSION;
    DevCaps->ulTechnology = DT_RASDISPLAY;
    DevCaps->ulHorzSize = 320;
    DevCaps->ulVertSize = 240;

    DevCaps->ulHorzRes = Pdev->ScreenWidth;
    DevCaps->ulVertRes = Pdev->ScreenHeight;
    DevCaps->ulPanningHorzRes = 0;
    DevCaps->ulPanningVertRes = 0;
    DevCaps->cBitsPixel = 32;
    DevCaps->cPlanes = 1;
    DevCaps->ulVRefresh = 1;	// not used
    DevCaps->ulBltAlignment = 1;	// We don't have accelerated screen-to-screen blts, and any window alignment is okay

    DevCaps->ulLogPixelsX = DevMode->dmLogPixels;
    DevCaps->ulLogPixelsY = DevMode->dmLogPixels;

    DevCaps->flTextCaps = TC_RA_ABLE;

    DevCaps->flRaster = 0;	// flRaster is reserved by DDI

    DevCaps->ulDACRed = 8;
    DevCaps->ulDACGreen = 8;
    DevCaps->ulDACBlue = 8;

    DevCaps->ulAspectX = 0x24;	// One-to-one aspect ratio
    DevCaps->ulAspectY = 0x24;
    DevCaps->ulAspectXY = 0x33;

    DevCaps->xStyleStep = 1;	// A style unit is 3 pels
    DevCaps->yStyleStep = 1;
    DevCaps->denStyleStep = 3;

    DevCaps->ptlPhysOffset.x = 0;
    DevCaps->ptlPhysOffset.y = 0;
    DevCaps->szlPhysSize.cx = 0;
    DevCaps->szlPhysSize.cy = 0;

    // RGB and CMY color info.

    DevCaps->ciDevice.Red.x = 6700;
    DevCaps->ciDevice.Red.y = 3300;
    DevCaps->ciDevice.Red.Y = 0;
    DevCaps->ciDevice.Green.x = 2100;
    DevCaps->ciDevice.Green.y = 7100;
    DevCaps->ciDevice.Green.Y = 0;
    DevCaps->ciDevice.Blue.x = 1400;
    DevCaps->ciDevice.Blue.y = 800;
    DevCaps->ciDevice.Blue.Y = 0;
    DevCaps->ciDevice.AlignmentWhite.x = 3127;
    DevCaps->ciDevice.AlignmentWhite.y = 3290;
    DevCaps->ciDevice.AlignmentWhite.Y = 0;

    DevCaps->ciDevice.RedGamma = 20000;
    DevCaps->ciDevice.GreenGamma = 20000;
    DevCaps->ciDevice.BlueGamma = 20000;

    DevCaps->ciDevice.Cyan.x = 0;
    DevCaps->ciDevice.Cyan.y = 0;
    DevCaps->ciDevice.Cyan.Y = 0;
    DevCaps->ciDevice.Magenta.x = 0;
    DevCaps->ciDevice.Magenta.y = 0;
    DevCaps->ciDevice.Magenta.Y = 0;
    DevCaps->ciDevice.Yellow.x = 0;
    DevCaps->ciDevice.Yellow.y = 0;
    DevCaps->ciDevice.Yellow.Y = 0;

    // No dye correction for raster displays.

    DevCaps->ciDevice.MagentaInCyanDye = 0;
    DevCaps->ciDevice.YellowInCyanDye = 0;
    DevCaps->ciDevice.CyanInMagentaDye = 0;
    DevCaps->ciDevice.YellowInMagentaDye = 0;
    DevCaps->ciDevice.CyanInYellowDye = 0;
    DevCaps->ciDevice.MagentaInYellowDye = 0;

    DevCaps->ulDevicePelsDPI = 0;	// For printers only
    DevCaps->ulPrimaryOrder = PRIMARY_ORDER_CBA;

    // Note: this should be modified later to take into account the size
    // of the display and the resolution.

    DevCaps->ulHTPatternSize = HT_PATSIZE_4x4_M;

    DevCaps->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;

    DevCaps->ulNumColors = (ULONG) -1;
    DevCaps->ulNumPalReg = 0;

    // Fill in the basic devinfo structure

    *DevInfo = gDevInfoFrameBuffer;

    switch (Pdev->BitsPerPel)
    {
    case 16:
        DevCaps->ulHTOutputFormat = HT_FORMAT_16BPP;
        DevInfo->iDitherFormat = BMF_16BPP;
        break;
    case 24:
        DevCaps->ulHTOutputFormat = HT_FORMAT_24BPP;
        DevInfo->iDitherFormat = BMF_24BPP;
        break;
    case 32:
        DevCaps->ulHTOutputFormat = HT_FORMAT_32BPP;
        DevInfo->iDitherFormat = BMF_32BPP;
        break;
    }

    DevInfo->hpalDefault = Pdev->DefaultPalette = EngCreatePalette(PAL_BITFIELDS, 0, NULL, 0xFF0000, 0xFF00, 0xFF);
    if (!DevInfo->hpalDefault)
        return FALSE;

    return TRUE;
}
