// default maximum refresh events per second
// 0 = disable limiter
#define DEFAULT_MAX_REFRESH_FPS 0LL

// upper limit
#define MAX_REFRESH_FPS 120LL

#define QVDISPLAY_TAG 'DDVQ'
#define DRIVER_NAME "QVIDEO"

__declspec(dllimport)
ULONG NTAPI DbgPrintEx(
    _In_ ULONG ComponentId,
    _In_ ULONG Level,
    _In_ PCSTR Format,
    ...
    );

// Following link explains configuration of filtering debug messages
// http://msdn.microsoft.com/en-us/library/windows/hardware/ff551519(v=vs.85).aspx
#define _DEBUGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL, format, ##__VA_ARGS__)

#define TRACEF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)
#define DEBUGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)
#define WARNINGF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)
#define ERRORF(format, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, \
    "[" DRIVER_NAME "] " __FUNCTION__ ": " format "\n", ##__VA_ARGS__)

#define FUNCTION_ENTER()   TRACEF("==>")
#define FUNCTION_EXIT()    TRACEF("<==")

VOID ReadRegistryConfig(VOID);

// returns number of changed pages
ULONG UpdateDirtyBits(
    void *va,
    ULONG size,
    QV_DIRTY_PAGES *pDirtyPages
    );

extern BOOLEAN g_bUseDirtyBits;
