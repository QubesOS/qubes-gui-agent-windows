#include <windows.h>

#include "common.h"
#include "hook-messages.h"

// This process serves only one purpose: to register 32-bit window hooks needed by the gui agent.
// Gui agent is 64-bit and therefore can't do this, but 32-bit hooks are needed
// to monitor 32-bit GUI processes.

// TODO: use common logger from windows-utils (would need to be built for 32bit...)
#define perror(x) (OutputDebugString(L"QGuiHookServer32: " L##x),GetLastError())

// This is not pretty but WDK 7.1 build requires that all source files for a project
// must be in one directory. Welcome to 1970's I suppose...
#include "..\gui-agent\register-hooks.c"

int __cdecl wmain(int argc, WCHAR* argv[])
{
    HANDLE shutdownEvent = OpenEvent(SYNCHRONIZE, FALSE, WGA_SHUTDOWN_EVENT_NAME);
    DWORD status;

    if (!shutdownEvent)
        return perror("OpenEvent(shutdown event)");

    status = SetHooks(HOOK_DLL_NAME_32);
    if (ERROR_SUCCESS != status)
        return status;

    // TODO: do we need a message pump here?
    
    return WaitForSingleObject(shutdownEvent, INFINITE);
    // Hooks are deleted when the owning thread terminates.
}
