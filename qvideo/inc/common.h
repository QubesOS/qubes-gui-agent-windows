#pragma once

#define PAGE_SIZE 0x1000

// registry configuration key, user mode and kernel mode names
#define REG_CONFIG_USER_KEY     L"Software\\Invisible Things Lab\\Qubes Tools"
#define REG_CONFIG_KERNEL_KEY   L"\\Registry\\Machine\\Software\\Invisible Things Lab\\Qubes Tools"

// value names in registry config
#define REG_CONFIG_LOG_VALUE        L"LogDir"
#define REG_CONFIG_FPS_VALUE        L"QvideoMaxFps"
#define REG_CONFIG_DIRTY_VALUE      L"UseDirtyBits"
#define REG_CONFIG_CURSOR_VALUE     L"DisableCursor"
#define REG_CONFIG_SEAMLESS_VALUE   L"SeamlessMode"

// path to the executable to launch at system start (done by helper service)
#define REG_CONFIG_AUTOSTART_VALUE  L"Autostart"

// event created by the helper service, trigger to simulate SAS (ctrl-alt-delete)
#define WGA_SAS_EVENT_NAME L"Global\\WGA_SAS_TRIGGER"

// When signaled, causes agent to shutdown gracefully.
#define WGA_SHUTDOWN_EVENT_NAME L"Global\\WGA_SHUTDOWN"

// Shutdown event for the 32-bit hook server.
#define WGA32_SHUTDOWN_EVENT_NAME L"Global\\WGA_HOOK32_SHUTDOWN"

// these are hardcoded
#define	MIN_RESOLUTION_WIDTH	320UL
#define	MIN_RESOLUTION_HEIGHT	200UL

#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))
#define	FRAMEBUFFER_PAGE_COUNT(width, height)	(ALIGN(((width)*(height)*4), PAGE_SIZE) / PAGE_SIZE)

#ifdef _X86_
typedef ULONG PFN_NUMBER;
#else
typedef ULONG64 PFN_NUMBER;
#endif

// size of PFN_ARRAY
#define PFN_ARRAY_SIZE(width, height) ((FRAMEBUFFER_PAGE_COUNT(width, height) * sizeof(PFN_NUMBER)) + sizeof(UINT32))

#pragma warning(push)
#pragma warning(disable: 4200) // zero-sized array

// NOTE: this struct is variable size, use FRAMEBUFFER_PAGE_COUNT
typedef struct _PFN_ARRAY
{
    ULONG NumberOf4kPages;
    PFN_NUMBER Pfn[0];
} PFN_ARRAY;

#pragma warning(pop)

// User mode -> display interface

#define	QVIDEO_MAGIC	0x49724515
#define	QVIDEO_ESC_BASE	0x11000

#define QVESC_SUPPORT_MODE		    (QVIDEO_ESC_BASE + 0)
#define QVESC_GET_SURFACE_DATA	    (QVIDEO_ESC_BASE + 1)
#define QVESC_WATCH_SURFACE		    (QVIDEO_ESC_BASE + 2)
#define QVESC_STOP_WATCHING_SURFACE	(QVIDEO_ESC_BASE + 3)
#define QVESC_GET_PFN_LIST		    (QVIDEO_ESC_BASE + 4)
#define QVESC_SYNCHRONIZE		    (QVIDEO_ESC_BASE + 5)

#define QV_SUCCESS	1
#define QV_INVALID_PARAMETER	2
#define QV_SUPPORT_MODE_INVALID_RESOLUTION	3
#define QV_SUPPORT_MODE_INVALID_BPP	4
#define QV_INVALID_HANDLE	5

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && (MIN_RESOLUTION_HEIGHT <= (uHeight)))

// wga-qvideo interface
typedef struct _QV_SUPPORT_MODE
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    ULONG Height;
    ULONG Width;
    ULONG Bpp;
} QV_SUPPORT_MODE;

typedef struct _QV_GET_SURFACE_DATA
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    // Must be allocated by the user mode client (use FRAMEBUFFER_PAGE_COUNT).
    // Driver will check if the size is sufficient and the buffer accessible.
    // Driver *could* return the correct size first but that would require
    // two calls because driver can't easily allocate user space memory.
    // Must be here instead of the response struct because GDI
    // doesn't copy contents of ExtEscape's output from user to kernel.
    PFN_ARRAY *PfnArray;

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_GET_SURFACE_DATA;

typedef struct _QV_GET_SURFACE_DATA_RESPONSE
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    ULONG Width;
    ULONG Height;
    ULONG Delta;
    ULONG Bpp;
    BOOLEAN IsScreen;
} QV_GET_SURFACE_DATA_RESPONSE;

typedef struct _QV_WATCH_SURFACE
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    HANDLE UserModeEvent;
} QV_WATCH_SURFACE;

typedef struct _QV_STOP_WATCHING_SURFACE
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_STOP_WATCHING_SURFACE;

// wga->display: confirmation that all dirty page data has been read
typedef struct _QV_SYNCHRONIZE
{
    ULONG Magic;		// must be present at the top of every QV_ structure

    // A surface handle is converted automatically by GDI from HDC to SURFOBJ, so do not pass it here
} QV_SYNCHRONIZE;

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
} QV_DIRTY_PAGES;
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

typedef struct _QVMINI_ALLOCATE_MEMORY
{
    ULONG Size;
} QVMINI_ALLOCATE_MEMORY;

typedef struct _QVMINI_ALLOCATE_MEMORY_RESPONSE
{
    void *VirtualAddress;
    PFN_ARRAY *PfnArray;
} QVMINI_ALLOCATE_MEMORY_RESPONSE;

typedef struct _QVMINI_FREE_MEMORY
{
    void *VirtualAddress;
    PFN_ARRAY *PfnArray;
} QVMINI_FREE_MEMORY;

typedef struct _QVMINI_ALLOCATE_SECTION
{
    ULONG Size;
    BOOLEAN UseDirtyBits;
} QVMINI_ALLOCATE_SECTION;

typedef struct _QVMINI_ALLOCATE_SECTION_RESPONSE
{
    void *VirtualAddress;
    void *SectionObject;
    HANDLE Section;
    void *Mdl;
    void *DirtySectionObject;
    HANDLE DirtySection;
    QV_DIRTY_PAGES *DirtyPages;
    PFN_ARRAY *PfnArray;
} QVMINI_ALLOCATE_SECTION_RESPONSE;

typedef struct _QVMINI_FREE_SECTION
{
    void *VirtualAddress;
    void *SectionObject;
    HANDLE Section;
    void *Mdl;
    void *DirtySectionObject;
    HANDLE DirtySection;
    QV_DIRTY_PAGES *DirtyPages;
    PFN_ARRAY *PfnArray;
} QVMINI_FREE_SECTION;
