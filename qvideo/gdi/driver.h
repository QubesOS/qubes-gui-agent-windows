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
	ULONG ulBitCount;	// # of bits per pel: 16, 24, 32 are only supported.

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
	PFN_ARRAY PfnArray;

	BITMAP_HEADER BitmapHeader;

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

#define DLL_NAME	L"QubesVideo"	// Name of the DLL in UNICODE
#define ALLOC_TAG	'DDVQ'	// Four byte tag (characters in
									 // reverse order) used for memory
									 // allocations
