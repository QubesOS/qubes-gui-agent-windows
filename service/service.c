#include <windows.h>
#include <WtsApi32.h>
#include <Shlwapi.h>
#include <string.h>
#include "..\qvideo\inc\common.h"
#include "log.h"
#include "config.h"

#define SERVICE_NAME L"QTWHelper"

#define WGA_TERMINATE_TIMEOUT 2000

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;
HANDLE g_ConsoleEvent;

void WINAPI ServiceMain(DWORD argc, WCHAR *argv[]);
DWORD WINAPI ControlHandlerEx(DWORD controlCode, DWORD eventType, void *eventData, void *context);

typedef void (WINAPI *SendSASFunction)(BOOL);
// this is not defined in WDK headers
SendSASFunction SendSAS = NULL;

WCHAR *g_SessionEventName[] = {
    L"<invalid>",
    L"WTS_CONSOLE_CONNECT",
    L"WTS_CONSOLE_DISCONNECT",
    L"WTS_REMOTE_CONNECT",
    L"WTS_REMOTE_DISCONNECT",
    L"WTS_SESSION_LOGON",
    L"WTS_SESSION_LOGOFF",
    L"WTS_SESSION_LOCK",
    L"WTS_SESSION_UNLOCK",
    L"WTS_SESSION_REMOTE_CONTROL",
    L"WTS_SESSION_CREATE",
    L"WTS_SESSION_TERMINATE"
};

// Held when restarting gui agent.
CRITICAL_SECTION wgaCs;

// Entry point.
int main(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRY	serviceTable[] = {
            { SERVICE_NAME, ServiceMain },
            { NULL, NULL }
    };

    InitializeCriticalSection(&wgaCs);
    StartServiceCtrlDispatcher(serviceTable);
    return 0;
}

BOOL IsProcessRunning(IN const WCHAR *exeName, OUT PDWORD processId OPTIONAL, OUT PDWORD sessionId OPTIONAL)
{
    WTS_PROCESS_INFO *processInfo = NULL;
    DWORD count = 0, i;
    HANDLE shutdownEvent = NULL;
    BOOL found = FALSE;

    // check if the shutdown event is present first
    shutdownEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, WGA_SHUTDOWN_EVENT_NAME);
    if (!shutdownEvent)
    {
        LogVerbose("Shutdown event not found");
        return FALSE;
    }
    CloseHandle(shutdownEvent);

    if (!WTSEnumerateProcesses(WTS_CURRENT_SERVER, 0, 1, &processInfo, &count))
    {
        perror("WTSEnumerateProcesses");
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

DWORD TerminateTargetProcess(WCHAR *exeName)
{
    HANDLE targetProcess;
    HANDLE shutdownEvent;
    DWORD processId, sessionId, status;

    status = GetLastError();
    LogInfo("Process name: '%s'", exeName);

    shutdownEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, WGA_SHUTDOWN_EVENT_NAME);
    if (!shutdownEvent)
    {
        perror("OpenEvent");
        LogInfo("Shutdown event '%s' not found, making sure it's not running", WGA_SHUTDOWN_EVENT_NAME);
    }
    else
    {
        LogDebug("Signaling shutdown event: %s", WGA_SHUTDOWN_EVENT_NAME);
        SetEvent(shutdownEvent);
        CloseHandle(shutdownEvent);

        LogDebug("Waiting for process shutdown");
    }

    if (IsProcessRunning(exeName, &processId, &sessionId))
    {
        LogDebug("Process '%s' running as PID %d in session %d, waiting for %dms",
            exeName, processId, sessionId, WGA_TERMINATE_TIMEOUT);

        targetProcess = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processId);

        if (!targetProcess)
        {
            return perror("OpenProcess");
        }

        // wait for exit
        if (WAIT_OBJECT_0 != WaitForSingleObject(targetProcess, WGA_TERMINATE_TIMEOUT))
        {
            LogWarning("Process didn't exit in time, killing it");
            TerminateProcess(targetProcess, 0);
        }
        CloseHandle(targetProcess);
    }
    return ERROR_SUCCESS;
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
        // we'll launch wga when the console connects to a session again
    }

    // Get access token from ourselves.
    OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &currenttSessionId, sizeof(currenttSessionId), &size);
    LogDebug("current session: %d, console session: %d", currenttSessionId, consoleSessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
    {
        return perror("DuplicateTokenEx");
    }
    CloseHandle(currentProcess);

    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &consoleSessionId, sizeof(consoleSessionId)))
    {
        return perror("SetTokenInformation(TokenSessionId)");
    }

    LogInfo("Running process '%s' in session %d", exePath, consoleSessionId);
    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    // No need to set desktop here, gui agent attaches to the input desktop anyway,
    // and hardcodint this to winlogon is wrong.
    if (!CreateProcessAsUser(newToken, NULL, exePath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        return perror("CreateProcessAsUser");
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return ERROR_SUCCESS;
}

// Restarts gui agent in active session if it's dead for too long.
DWORD WINAPI WatchdogThread(void *param)
{
    WCHAR *cmdline;
    WCHAR *exeName;

    cmdline = (WCHAR*) param;
    PathUnquoteSpaces(cmdline);
    exeName = PathFindFileName(cmdline);

    LogDebug("cmdline: '%s', exe: '%s'", cmdline, exeName);

    while (TRUE)
    {
        Sleep(2000);

        EnterCriticalSection(&wgaCs);
        // Check if the gui agent is running.
        if (!IsProcessRunning(exeName, NULL, NULL))
        {
            LogWarning("Process '%s' not running, restarting it", exeName);
            StartTargetProcess(cmdline);
        }
        LeaveCriticalSection(&wgaCs);
    }

    return ERROR_SUCCESS;
}

DWORD WINAPI SessionChangeThread(void *param)
{
    WCHAR *cmdline;
    WCHAR *exeName;
    HANDLE events[2];
    DWORD signaledEvent = 3;

    LogDebug("start");

    cmdline = (WCHAR*) param;
    PathUnquoteSpaces(cmdline);
    exeName = PathFindFileName(cmdline);

    LogDebug("cmdline: '%s', exe: '%s'", cmdline, exeName);

    events[0] = g_ConsoleEvent;
    // Default security for the SAS event, only SYSTEM processes can signal it.
    events[1] = CreateEvent(NULL, FALSE, FALSE, WGA_SAS_EVENT_NAME);

    while (TRUE)
    {
        // Wait until the interactive session changes.
        signaledEvent = WaitForMultipleObjects(2, events, FALSE, INFINITE) - WAIT_OBJECT_0;

        switch (signaledEvent)
        {
        case 0: // console event: restart wga
            EnterCriticalSection(&wgaCs);
            // Make sure process is not running.
            TerminateTargetProcess(exeName);
            // restart
            StartTargetProcess(cmdline);
            LeaveCriticalSection(&wgaCs);
            break;

        case 1: // SAS event
            LogInfo("SAS event signaled");
            if (SendSAS)
                SendSAS(FALSE); // calling as service
            break;

        default:
            LogWarning("Wait failed, result 0x%x", signaledEvent + WAIT_OBJECT_0);
        }
    }

    return ERROR_SUCCESS;
}

void WINAPI ServiceMain(DWORD argc, WCHAR *argv[])
{
    WCHAR cmdline1[MAX_PATH], cmdline2[MAX_PATH];
    WCHAR moduleName[CFG_MODULE_MAX];
    HANDLE workerHandle = NULL;
    HANDLE watchdogHandle = NULL;
    DWORD status;
    HANDLE sasDll;

    // Read the registry configuration.
    CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));
    status = CfgReadString(moduleName, REG_CONFIG_AUTOSTART_VALUE, cmdline1, RTL_NUMBER_OF(cmdline1), NULL);
    if (ERROR_SUCCESS != status)
    {
        perror("RegQueryValueEx(Autostart)");
        goto cleanup;
    }

    // 2nd copy is for the 2nd thread since it will be modified by Path* functions
    memcpy(cmdline2, cmdline1, sizeof(cmdline2));

    // Get SendSAS address.
    sasDll = LoadLibrary(L"sas.dll");
    if (sasDll)
    {
        SendSAS = (SendSASFunction) GetProcAddress(sasDll, "SendSAS");
        if (!SendSAS)
            LogWarning("Failed to get SendSAS() address, simulating CTRL+ALT+DELETE will not be possible");
    }
    else
    {
        LogWarning("Failed to load sas.dll, simulating CTRL+ALT+DELETE will not be possible");
    }

    g_Status.dwServiceType = SERVICE_WIN32;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    // SERVICE_ACCEPT_SESSIONCHANGE allows us to receive session change notifications.
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    g_Status.dwWin32ExitCode = 0;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 0;
    g_StatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, ControlHandlerEx, NULL);
    if (g_StatusHandle == 0)
    {
        perror("RegisterServiceCtrlHandlerEx");
        goto cleanup;
    }

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    // Create trigger event for the worker thread.
    g_ConsoleEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Start the worker thread.
    LogDebug("Starting worker thread");
    workerHandle = CreateThread(NULL, 0, SessionChangeThread, cmdline1, 0, NULL);
    if (!workerHandle)
    {
        perror("CreateThread");
        goto cleanup;
    }

    LogDebug("Starting watchdog thread");
    watchdogHandle = CreateThread(NULL, 0, WatchdogThread, cmdline2, 0, NULL);
    if (!watchdogHandle)
    {
        perror("CreateThread");
        goto cleanup;
    }

    // Start the gui agent.
    StartTargetProcess(cmdline1);

    // Wait for the worker thread to exit.
    WaitForSingleObject(workerHandle, INFINITE);

cleanup:
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = GetLastError();
    SetServiceStatus(g_StatusHandle, &g_Status);

    LogInfo("exiting");
    return;
}

void SessionChange(DWORD eventType, WTSSESSION_NOTIFICATION *sn)
{
    static DWORD previousConsoleId = 0;
    DWORD consoleId = WTSGetActiveConsoleSessionId();

    if (eventType < RTL_NUMBER_OF(g_SessionEventName))
        LogDebug("%s, session ID %d, console session: %d", g_SessionEventName[eventType], sn->dwSessionId, consoleId);
    else
        LogDebug("<unknown event: %d>, session ID %d, console session: %d", eventType, sn->dwSessionId, consoleId);

    if (consoleId != previousConsoleId)
    {
        LogDebug("console session change, signaling event");
        previousConsoleId = consoleId;
        // Signal trigger event for the worker thread.
        SetEvent(g_ConsoleEvent);
    }
}

DWORD WINAPI ControlHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    switch (dwControl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOPPED;
        LogInfo("stopping...");
        SetServiceStatus(g_StatusHandle, &g_Status);
        break;

    case SERVICE_CONTROL_SESSIONCHANGE:
        SessionChange(dwEventType, (WTSSESSION_NOTIFICATION*) lpEventData);
        break;

    default:
        LogDebug("code 0x%x, event 0x%x", dwControl, dwEventType);
        break;
    }

    return ERROR_SUCCESS;
}
