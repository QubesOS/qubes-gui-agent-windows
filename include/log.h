#pragma once

// TODO: copyright notice goes here

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>

#ifdef DBG
# define LOG_FILE		TEXT("c:\\windows_gui_agent.log")
#endif

/*
 * Logging functions
 */

#ifdef DBG

VOID Lprintf(
	PUCHAR szFormat,
	...
);
VOID Lprintf_err(
	ULONG uErrorCode,
	PUCHAR szFormat,
	...
);

VOID logprintf(
	PUCHAR szFormat,
	...
);

#else

# define Lprintf(...)
# define Lprintf_err(...)
# define logprintf(...)

#endif
