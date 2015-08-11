#include <stddef.h>
#include <stdarg.h>

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)

#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>

#pragma warning(pop)
// C4200: nonstandard extension used :
//        zero-sized array in struct/union
// C4201: nonstandard extension used:
//        nameless struct/union
// C4214: nonstandard extension used:
//        bit field types other than int

#include "debug.h"
#include "common.h"

typedef struct _QV_SURFACE QV_SURFACE;
typedef struct _QV_PDEV
{
    HANDLE DisplayHandle;    // Handle to \Device\Screen.
    HDEV EngPdevHandle;      // Engine's handle to PDEV.
    HSURF EngSurfaceHandle;  // Engine's handle to screen surface.
    HPALETTE DefaultPalette; // Handle to the default palette for device.

    ULONG ScreenWidth;       // Visible screen width.
    ULONG ScreenHeight;      // Visible screen height.
    LONG ScreenDelta;        // Distance from one scan line to the next.
    ULONG BitsPerPel;        // Number of bits per pel: only 16, 24, 32 are supported.

    QV_SURFACE *ScreenSurface; // Pointer to screen surface data.
} QV_PDEV, *PQV_PDEV;

#pragma pack(push, 1)
typedef struct _BITMAP_HEADER
{
    BITMAPFILEHEADER FileHeader;
    BITMAPV5HEADER V5Header;
} BITMAP_HEADER;
#pragma pack(pop)

typedef struct _QV_SURFACE
{
    ULONG Width;
    ULONG Height;
    ULONG Delta;
    ULONG BitCount;
    BOOLEAN IsScreen;

    PQV_PDEV Pdev;
    HDRVOBJ DriverObj;
    PEVENT DamageNotificationEvent;

    PVOID PixelData;
    HANDLE Section;
    PVOID SectionObject;
    PVOID Mdl;
    PFN_ARRAY *PfnArray; // this is allocated by the miniport part

    // page numbers that changed in the surface buffer since the last check
    // this is exposed as a section so the user mode client can easily check what changed
    PVOID DirtySectionObject;
    HANDLE DirtySection;
    QV_DIRTY_PAGES *DirtyPages;
} QV_SURFACE, *PQV_SURFACE;

BOOL InitPdev(
    QV_PDEV *,
    DEVMODEW *,
    GDIINFO *,
    DEVINFO *
    );

// Name of the DLL in UNICODE
#define DLL_NAME	L"QubesVideo"

// Four byte tag (characters in reverse order) used for memory allocations
#define ALLOC_TAG	'DDVQ'
