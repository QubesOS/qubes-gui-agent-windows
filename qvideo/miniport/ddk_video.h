#include <ntddk.h>

//
// Debugging statements. This will remove all the debug information from the
// "free" version.
//

#if DBG
# define VideoDebugPrint(arg) VideoPortDebugPrint arg
#else
# define VideoDebugPrint(arg)
#endif

typedef enum VIDEO_DEBUG_LEVEL
{
	Error = 0,
	Warn,
	Trace,
	Info
} VIDEO_DEBUG_LEVEL, *PVIDEO_DEBUG_LEVEL;

#define VIDEOPORT_API __declspec(dllimport)

VIDEOPORT_API VOID VideoPortDebugPrint(
	VIDEO_DEBUG_LEVEL DebugPrintLevel,
	__in PSTR DebugMessage,
	...
);
