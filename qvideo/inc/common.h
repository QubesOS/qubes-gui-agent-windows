#pragma once

#define PAGE_SIZE 0x1000

#define	MAX_RESOLUTION_WIDTH	4000
#define	MAX_RESOLUTION_HEIGHT	4000

#define	MIN_RESOLUTION_WIDTH	320
#define	MIN_RESOLUTION_HEIGHT	200

#define	MAX_RETURNED_PFNS	(((MAX_RESOLUTION_WIDTH*MAX_RESOLUTION_HEIGHT*4) / PAGE_SIZE) + 1)

#ifdef _X86_
typedef ULONG PFN_NUMBER, *PPFN_NUMBER;
#else
typedef ULONG64 PFN_NUMBER, *PPFN_NUMBER;
#endif

typedef struct _PFN_ARRAY
{
    ULONG uNumberOf4kPages;
    PFN_NUMBER Pfn[MAX_RETURNED_PFNS];
} PFN_ARRAY, *PPFN_ARRAY;


// User mode -> display interface

#define	QVIDEO_MAGIC	0x49724515
#define	QVIDEO_ESC_BASE	0x11000

#define QVESC_SUPPORT_MODE		(QVIDEO_ESC_BASE + 0)
#define QVESC_GET_SURFACE_DATA	(QVIDEO_ESC_BASE + 1)
#define QVESC_WATCH_SURFACE		(QVIDEO_ESC_BASE + 2)
#define QVESC_STOP_WATCHING_SURFACE	(QVIDEO_ESC_BASE + 3)
#define QVESC_GET_PFN_LIST		(QVIDEO_ESC_BASE + 4)
#define QVESC_SYNCHRONIZE		(QVIDEO_ESC_BASE + 5)

#define QV_SUCCESS	1
#define QV_INVALID_PARAMETER	2
#define QV_SUPPORT_MODE_INVALID_RESOLUTION	3
#define QV_SUPPORT_MODE_INVALID_BPP	4
#define QV_INVALID_HANDLE	5

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && ((uWidth) <= MAX_RESOLUTION_WIDTH) && (MIN_RESOLUTION_HEIGHT <= (uHeight)) && ((uHeight) <= MAX_RESOLUTION_HEIGHT))
#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))

typedef struct _QV_SUPPORT_MODE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    ULONG uHeight;
    ULONG uWidth;
    ULONG uBpp;
} QV_SUPPORT_MODE, *PQV_SUPPORT_MODE;

typedef struct _QV_GET_SURFACE_DATA
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_GET_SURFACE_DATA, *PQV_GET_SURFACE_DATA;

typedef struct _QV_GET_SURFACE_DATA_RESPONSE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    ULONG cx;
    ULONG cy;
    ULONG lDelta;
    ULONG ulBitCount;
    BOOLEAN bIsScreen;

    PFN_ARRAY PfnArray;
} QV_GET_SURFACE_DATA_RESPONSE, *PQV_GET_SURFACE_DATA_RESPONSE;

typedef struct _QV_GET_PFN_LIST
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    PVOID pVirtualAddress;
    ULONG uRegionSize;
} QV_GET_PFN_LIST, *PQV_GET_PFN_LIST;

typedef struct _QV_GET_PFN_LIST_RESPONSE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    PFN_ARRAY PfnArray;
} QV_GET_PFN_LIST_RESPONSE, *PQV_GET_PFN_LIST_RESPONSE;

typedef struct _QV_WATCH_SURFACE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    HANDLE hUserModeEvent;
} QV_WATCH_SURFACE, *PQV_WATCH_SURFACE;

typedef struct _QV_STOP_WATCHING_SURFACE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_STOP_WATCHING_SURFACE, *PQV_STOP_WATCHING_SURFACE;

// wga->display: confirmation that all dirty page data has been read
typedef struct _QV_SYNCHRONIZE
{
    ULONG uMagic;		// must be present at the top of every QV_ structure

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_SYNCHRONIZE, *PQV_SYNCHRONIZE;

#pragma warning(push)
#pragma warning(disable: 4200) // zero-sized array
// Structure describing dirty pages of surface memory.
// Maintained by the display driver, mapped as QvideoDirtyPages_* section.
typedef struct _QV_DIRTY_PAGES
{
    // User mode client sets this to 1 after it reads the data (indirectly by QVESC_SYNCHRONIZE).
    // Driver overwrites the dirty bitfield with fresh data if this is 1 (and sets it to 0).
    // If this is 0, driver ORs the bitfield with new data to accumulate changes
    // until the client reads everything.
    LONG Ready;

    // Bitfield describing which surface memory pages changed since the last check.
    // number_of_pages = screen_width*screen_height*4 //32bpp
    // Size of DirtyBits array (in bytes) = (number_of_pages >> 3) + 1
    // Bit set means that the corresponding memory page has changed.
    UCHAR DirtyBits[0];
} QV_DIRTY_PAGES, *PQV_DIRTY_PAGES;
#pragma warning(pop)

#define BIT_GET(array, bit_number) (array[(bit_number)/8] & (1 << ((bit_number) % 8)))
#define BIT_SET(array, bit_number) (array[(bit_number)/8] |= (1 << ((bit_number) % 8)))
#define BIT_CLEAR(array, bit_number) (array[(bit_number)/8] &= ~(1 << ((bit_number) % 8)))

// Display -> Miniport interface

// TODO? all those functions can be moved to display since we link with kernel anyway now...
#define	QVMINI_DEVICE	0x0a000

#define IOCTL_QVMINI_ALLOCATE_MEMORY    (ULONG)(CTL_CODE(QVMINI_DEVICE, 1, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_FREE_MEMORY        (ULONG)(CTL_CODE(QVMINI_DEVICE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_ALLOCATE_SECTION   (ULONG)(CTL_CODE(QVMINI_DEVICE, 3, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_FREE_SECTION       (ULONG)(CTL_CODE(QVMINI_DEVICE, 4, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_GET_PFN_LIST       (ULONG)(CTL_CODE(QVMINI_DEVICE, 5, METHOD_BUFFERED, FILE_ANY_ACCESS))

typedef struct _QVMINI_ALLOCATE_MEMORY
{
    ULONG uLength;
} QVMINI_ALLOCATE_MEMORY, *PQVMINI_ALLOCATE_MEMORY;

typedef struct _QVMINI_ALLOCATE_MEMORY_RESPONSE
{
    PVOID pVirtualAddress;
    PFN_ARRAY PfnArray;
} QVMINI_ALLOCATE_MEMORY_RESPONSE, *PQVMINI_ALLOCATE_MEMORY_RESPONSE;

typedef struct _QVMINI_FREE_MEMORY
{
    PVOID pVirtualAddress;
} QVMINI_FREE_MEMORY, *PQVMINI_FREE_MEMORY;

typedef struct _QVMINI_ALLOCATE_SECTION
{
    ULONG uLength;
    BOOLEAN bUseDirtyBits;
} QVMINI_ALLOCATE_SECTION, *PQVMINI_ALLOCATE_SECTION;

typedef struct _QVMINI_ALLOCATE_SECTION_RESPONSE
{
    PVOID pVirtualAddress;
    PVOID SectionObject;
    HANDLE hSection;
    PVOID pMdl;
    PFN_ARRAY PfnArray;
    PVOID DirtySectionObject;
    HANDLE hDirtySection;
    PQV_DIRTY_PAGES pDirtyPages;
} QVMINI_ALLOCATE_SECTION_RESPONSE, *PQVMINI_ALLOCATE_SECTION_RESPONSE;

typedef struct _QVMINI_FREE_SECTION
{
    PVOID pVirtualAddress;
    PVOID SectionObject;
    HANDLE hSection;
    PVOID pMdl;
    PVOID DirtySectionObject;
    HANDLE hDirtySection;
    PQV_DIRTY_PAGES pDirtyPages;
} QVMINI_FREE_SECTION, *PQVMINI_FREE_SECTION;

typedef struct _QVMINI_GET_PFN_LIST
{
    PVOID pVirtualAddress;
    ULONG uRegionSize;
} QVMINI_GET_PFN_LIST, *PQVMINI_GET_PFN_LIST;

typedef struct _QVMINI_GET_PFN_LIST_RESPONSE
{
    PFN_ARRAY PfnArray;
} QVMINI_GET_PFN_LIST_RESPONSE, *PQVMINI_GET_PFN_LIST_RESPONSE;
