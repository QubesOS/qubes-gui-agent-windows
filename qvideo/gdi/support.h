// default maximum refresh events per second
// 0 = disable limiter
#define DEFAULT_MAX_REFRESH_FPS 0LL

// upper limit
#define MAX_REFRESH_FPS 120LL

#define QVDISPLAY_TAG 'DDVQ'
#define DRIVER_NAME "QVIDEO"

// Following link explains configuration of filtering debug messages
// http://msdn.microsoft.com/en-us/library/windows/hardware/ff551519(v=vs.85).aspx
#define _DEBUGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, format, ##__VA_ARGS__)
#define DEBUGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)
#define WARNINGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)
#define ERRORF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)

VOID ReadRegistryConfig(VOID);

// returns number of changed pages
ULONG UpdateDirtyBits(
    PVOID va,
    ULONG size,
    PQV_DIRTY_PAGES pDirtyPages,
    IN OUT PLARGE_INTEGER pTimestamp
    );

extern BOOLEAN g_bUseDirtyBits;
