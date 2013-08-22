// Enough to describe 20480000 4-byte pixels, which is roughly 4525x4525.
#define	MAX_RETURNED_PFNS	20000

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
#define QVESC_GET_SURFACE_DATA		(QVIDEO_ESC_BASE + 1)
#define QVESC_WATCH_SURFACE		(QVIDEO_ESC_BASE + 2)
#define QVESC_STOP_WATCHING_SURFACE	(QVIDEO_ESC_BASE + 3)

#define QV_SUCCESS	1
#define QV_INVALID_PARAMETER	2
#define QV_SUPPORT_MODE_INVALID_RESOLUTION	3
#define QV_SUPPORT_MODE_INVALID_BPP	4
#define QV_INVALID_HANDLE	5

#define	MAX_RESOLUTION_WIDTH	2000
#define	MAX_RESOLUTION_HEIGHT	2000

#define	MIN_RESOLUTION_WIDTH	640
#define	MIN_RESOLUTION_HEIGHT	480

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && ((uWidth) <= MAX_RESOLUTION_WIDTH) && (MIN_RESOLUTION_HEIGHT <= (uHeight)) && ((uHeight) <= MAX_RESOLUTION_HEIGHT))
#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))

typedef struct _QV_SUPPORT_MODE
{
	ULONG uMagic;		// must be present at the top of every QV_ structure

	ULONG uHeight;
	ULONG uWidth;
	UCHAR uBpp;

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

// Display -> Miniport interface

#define	QVMINI_DEVICE	0x0a000

#define IOCTL_QVMINI_ALLOCATE_MEMORY	(unsigned long)(CTL_CODE(QVMINI_DEVICE, 1, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_FREE_MEMORY	(unsigned long)(CTL_CODE(QVMINI_DEVICE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_ALLOCATE_SECTION	(unsigned long)(CTL_CODE(QVMINI_DEVICE, 3, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_QVMINI_FREE_SECTION	(unsigned long)(CTL_CODE(QVMINI_DEVICE, 4, METHOD_BUFFERED, FILE_ANY_ACCESS))

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
} QVMINI_ALLOCATE_SECTION, *PQVMINI_ALLOCATE_SECTION;

typedef struct _QVMINI_ALLOCATE_SECTION_RESPONSE
{
	PVOID pVirtualAddress;
	PVOID SectionObject;
	HANDLE hSection;
	PVOID pMdl;
	PFN_ARRAY PfnArray;
} QVMINI_ALLOCATE_SECTION_RESPONSE, *PQVMINI_ALLOCATE_SECTION_RESPONSE;

typedef struct _QVMINI_FREE_SECTION
{
	PVOID pVirtualAddress;
	PVOID SectionObject;
	HANDLE hSection;
	PVOID pMdl;
} QVMINI_FREE_SECTION, *PQVMINI_FREE_SECTION;
