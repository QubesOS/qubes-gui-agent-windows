#include "stddef.h"

#include <stdarg.h>

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)

#include "windef.h"
#include "wingdi.h"
#include "winddi.h"
#include "devioctl.h"
#include "ntddvdeo.h"

#pragma warning(pop)		// C4200: nonstandard extension used :
                    //        zero-sized array in struct/union
                    // C4201: nonstandard extension used:
                    //        nameless struct/union
                    // C4214: nonstandard extension used:
                    //        bit field types other than int

#include "debug.h"
#include "common.h"

typedef struct _SURFACE_DESCRIPTOR SURFACE_DESCRIPTOR, *PSURFACE_DESCRIPTOR;
typedef struct _PDEV
{
    HANDLE hDriver;		// Handle to \Device\Screen
    HDEV hdevEng;		// Engine's handle to PDEV
    HSURF hsurfEng;		// Engine's handle to surface
    HPALETTE hpalDefault;	// Handle to the default palette for device.

    ULONG cxScreen;		// Visible screen width
    ULONG cyScreen;		// Visible screen height

    LONG lDeltaScreen;	// Distance from one scan to the next.
    ULONG ulBitCount;	// # of bits per pel: only 16, 24, 32 are supported.

    PSURFACE_DESCRIPTOR pScreenSurfaceDescriptor;	// ptr to SURFACE_DESCRIPTOR bits for screen surface
} PDEV, *PPDEV;

#pragma pack(push, 1)
typedef struct _BITMAP_HEADER
{
    BITMAPFILEHEADER FileHeader;
    BITMAPV5HEADER V5Header;
} BITMAP_HEADER, *PBITMAP_HEADER;
#pragma pack(pop)

typedef struct _SURFACE_DESCRIPTOR
{
    ULONG cx;
    ULONG cy;
    ULONG lDelta;
    ULONG ulBitCount;
    BOOLEAN bIsScreen;

    PPDEV ppdev;
    HDRVOBJ hDriverObj;
    PEVENT pDamageNotificationEvent;

    PVOID pSurfaceData;
    HANDLE hSection;
    PVOID SectionObject;
    PVOID pMdl;
    PPFN_ARRAY pPfnArray; // this is allocated by the miniport part

    // page numbers that changed in the surface buffer since the last check
    // this is exposed as a section so the user mode client can easily check what changed
    PVOID DirtySectionObject;
    HANDLE hDirtySection;
    PQV_DIRTY_PAGES pDirtyPages;
    LARGE_INTEGER LastCheck; // timestamp of the last dirty pages check, to limit events per second

//  BITMAP_HEADER BitmapHeader;
} SURFACE_DESCRIPTOR, *PSURFACE_DESCRIPTOR;

BOOL bInitPDEV(
    PPDEV,
    PDEVMODEW,
    GDIINFO *,
    DEVINFO *
);

//
// Determines the size of the DriverExtra information in the DEVMODE
// structure passed to and from the display driver.
//

//#define DRIVER_EXTRA_SIZE 0

// Name of the DLL in UNICODE
#define DLL_NAME	L"QubesVideo"

// Four byte tag (characters in reverse order) used for memory allocations
#define ALLOC_TAG	'DDVQ'
