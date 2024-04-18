/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>
#include <wtsapi32.h>
#include <sas.h>
#include <shlwapi.h>
#include <strsafe.h>
#include "common.h"

#include <log.h>
#include <config.h>
#include <qubes-io.h>

#define SERVICE_NAME L"QgaWatchdog"

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;

void WINAPI ServiceMain(IN DWORD argc, IN WCHAR *argv[]);
DWORD WINAPI ControlHandlerEx(IN DWORD controlCode, IN DWORD eventType, IN void *eventData, IN void *context);

// Entry point.
int wmain(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRY	serviceTable[] = {
        { SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    StartServiceCtrlDispatcher(serviceTable);
    return ERROR_SUCCESS;
}

BOOL IsProcessRunning(IN const WCHAR *exeName, OUT DWORD *processId OPTIONAL, OUT DWORD *sessionId OPTIONAL)
{
    WTS_PROCESS_INFO *processInfo = NULL;
    DWORD count = 0, i;
    HANDLE shutdownEvent = NULL;
    BOOL found = FALSE;

    if (!WTSEnumerateProcesses(WTS_CURRENT_SERVER, 0, 1, &processInfo, &count))
    {
        win_perror("WTSEnumerateProcesses");
        goto cleanup;
    }

    for (i = 0; i < count; i++)
    {
        if (0 == _wcsnicmp(exeName, processInfo[i].pProcessName, wcslen(exeName))) // match
        {
            if (processId)
                *processId = processInfo[i].ProcessId;
            if (sessionId)
                *sessionId = processInfo[i].SessionId;
            LogVerbose("%s: PID %d, session %d", processInfo[i].pProcessName, processInfo[i].ProcessId, processInfo[i].SessionId);
            found = TRUE;
            break;
        }
    }

cleanup:
    if (processInfo)
        WTSFreeMemory(processInfo);
    return found;
}

// Starts the process as SYSTEM in currently active console session.
DWORD StartTargetProcess(IN WCHAR *exePath) // non-const because it can be modified by CreateProcess*
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD currenttSessionId, consoleSessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    consoleSessionId = WTSGetActiveConsoleSessionId();
    if (consoleSessionId == 0xFFFFFFFF) // disconnected or changing
    {
        LogDebug("console session is 0x%x, skipping", consoleSessionId);
        return ERROR_SUCCESS;
        // we'll launch gui agent when the console connects to a session again
    }

    // Get access token from ourselves.
    OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &currenttSessionId, sizeof(currenttSessionId), &size);
    LogDebug("current session: %d, console session: %d", currenttSessionId, consoleSessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
    {
        return win_perror("DuplicateTokenEx");
    }
    CloseHandle(currentProcess);

    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &consoleSessionId, sizeof(consoleSessionId)))
    {
        return win_perror("SetTokenInformation(TokenSessionId)");
    }

    LogInfo("Running process '%s' in session %d", exePath, consoleSessionId);
    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    // No need to set desktop here, gui agent attaches to the input desktop anyway,
    // and hardcoding this to winlogon is wrong.
    if (!CreateProcessAsUser(newToken, NULL, exePath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        return win_perror("CreateProcessAsUser");
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return ERROR_SUCCESS;
}

// Restarts gui agent in active session if it's dead for too long.
DWORD WINAPI WatchdogThread(void *param)
{
    WCHAR* cmdline = (WCHAR*) param;

    PathUnquoteSpaces(cmdline);
    WCHAR* exeName = PathFindFileName(cmdline);

    LogDebug("cmdline: '%s', exe: '%s'", cmdline, exeName);

    while (TRUE)
    {
        Sleep(1000);

        // Check if the gui agent is running.
        if (!IsProcessRunning(exeName, NULL, NULL))
        {
            LogWarning("Process '%s' not running, restarting it", exeName);
            StartTargetProcess(cmdline);
        }
    }

    return ERROR_SUCCESS;
}

DWORD WINAPI EventsThread(void *param)
{
    HANDLE events[1];
    DWORD signaledEvent = 2;

    LogDebug("start");

    // Default security for the SAS event, only SYSTEM processes can signal it.
    events[0] = CreateEvent(NULL, FALSE, FALSE, QGA_SAS_EVENT_NAME);

    while (TRUE)
    {
        signaledEvent = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) - WAIT_OBJECT_0;

        switch (signaledEvent)
        {
        case 0: // SAS event
            LogInfo("SAS event signaled");
            SendSAS(FALSE); // calling as service
            break;

        default:
            LogWarning("Wait failed, result 0x%x", signaledEvent + WAIT_OBJECT_0);
        }
    }

    return ERROR_SUCCESS;
}

void WINAPI ServiceMain(IN DWORD argc, IN WCHAR *argv[])
{
    WCHAR moduleName[CFG_MODULE_MAX];
    HANDLE workerHandle = NULL;
    HANDLE watchdogHandle = NULL;
    DWORD status;

    WCHAR* cmdline = malloc(MAX_PATH_LONG_WSIZE);
    if (!cmdline)
        goto cleanup;

    // Read the registry configuration.
    CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));
    status = CfgReadString(moduleName, REG_CONFIG_AGENT_PATH_VALUE, cmdline, MAX_PATH_LONG, NULL);
    if (ERROR_SUCCESS != status)
    {
        win_perror("CfgReadString(" REG_CONFIG_AGENT_PATH_VALUE L")");
        goto cleanup;
    }

    g_Status.dwServiceType = SERVICE_WIN32;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_Status.dwWin32ExitCode = 0;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 0;
    g_StatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, ControlHandlerEx, NULL);
    if (g_StatusHandle == 0)
    {
        win_perror("RegisterServiceCtrlHandlerEx");
        goto cleanup;
    }

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    LogDebug("Starting event thread");
    workerHandle = CreateThread(NULL, 0, EventsThread, NULL, 0, NULL);
    if (!workerHandle)
    {
        win_perror("CreateThread(events)");
        goto cleanup;
    }

    LogDebug("Starting watchdog thread");
    watchdogHandle = CreateThread(NULL, 0, WatchdogThread, cmdline, 0, NULL);
    if (!watchdogHandle)
    {
        win_perror("CreateThread(watchdog)");
        goto cleanup;
    }

    // FIXME that thread never exits
    WaitForSingleObject(workerHandle, INFINITE);

cleanup:
    // don't free cmdline here, a thread using it may be still running, memory is freed on exit anyway
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = GetLastError();
    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);

    LogInfo("exiting");
    return;
}

DWORD WINAPI ControlHandlerEx(IN DWORD controlCode, IN DWORD eventType, IN void *eventData, IN void *context)
{
    switch (controlCode)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOPPED;
        LogInfo("stopping...");
        SetServiceStatus(g_StatusHandle, &g_Status);
        break;
    default:
        LogDebug("code 0x%x, event 0x%x", controlCode, eventType);
        break;
    }

    return ERROR_SUCCESS;
}
