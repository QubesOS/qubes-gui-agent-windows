#include <windows.h>
#include <WtsApi32.h>
#include <Shlwapi.h>
#include <string.h>
#include "..\qvideo\inc\common.h"
#include "log.h"
#include "config.h"

#define SERVICE_NAME L"QTWHelper"

#define WGA_TERMINATE_TIMEOUT 5000
// FIXME: move shared definitions to one common file

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

// Entry point.
int main(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRY	serviceTable[] = {
            { SERVICE_NAME, ServiceMain },
            { NULL, NULL }
    };

    StartServiceCtrlDispatcher(serviceTable);
}

void TerminateTargetProcess(WCHAR *exeName)
{
    WTS_PROCESS_INFO *processInfo = NULL;
    DWORD count = 0, i;
    HANDLE targetProcess;
    HANDLE shutdownEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, WGA_SHUTDOWN_EVENT_NAME);

    LogInfo("Process name: %s", exeName);
    if (!shutdownEvent)
    {
        LogDebug("Shutdown event '%s' not found, making sure it's not running", WGA_SHUTDOWN_EVENT_NAME);
    }
    else
    {
        LogDebug("Signaling shutdown event: %s", WGA_SHUTDOWN_EVENT_NAME);
        SetEvent(shutdownEvent);
        CloseHandle(shutdownEvent);

        LogDebug("Waiting for process shutdown");
    }

    if (!WTSEnumerateProcesses(WTS_CURRENT_SERVER, 0, 1, &processInfo, &count))
    {
        perror("WTSEnumerateProcesses");
        goto cleanup;
    }

    for (i = 0; i < count; i++)
    {
        if (0 == _wcsnicmp(exeName, processInfo[i].pProcessName, wcslen(exeName))) // match
        {
            LogDebug("Process '%s' running as PID %d in session %d, waiting for %dms",
                exeName, processInfo[i].ProcessId, processInfo[i].SessionId, WGA_TERMINATE_TIMEOUT);
            targetProcess = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processInfo[i].ProcessId);
            if (!targetProcess)
            {
                perror("OpenProcess");
                goto cleanup;
            }

            // wait for exit
            if (WAIT_OBJECT_0 != WaitForSingleObject(targetProcess, WGA_TERMINATE_TIMEOUT))
            {
                LogWarning("Process didn't exit in time, killing it");
                TerminateProcess(targetProcess, 0);
            }
            CloseHandle(targetProcess);
            break;
        }
    }

cleanup:
    if (processInfo)
        WTSFreeMemory(processInfo);
}

DWORD WINAPI WorkerThread(void *param)
{
    WCHAR *cmdline;
    WCHAR *exeName;
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD sessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();
    HANDLE events[2];
    DWORD signaledEvent = 3;

    cmdline = (WCHAR*) param;
    PathUnquoteSpaces(cmdline);
    exeName = PathFindFileName(cmdline);

    events[0] = g_ConsoleEvent;
    // Default security for the SAS event, only SYSTEM processes can signal it.
    events[1] = CreateEvent(NULL, FALSE, FALSE, WGA_SAS_EVENT_NAME);

    LogDebug("start");

    while (1)
    {
        // Wait until the interactive session changes (to winlogon console).
        signaledEvent = WaitForMultipleObjects(2, events, FALSE, INFINITE) - WAIT_OBJECT_0;

        switch (signaledEvent)
        {
        case 0: // console event: restart wga
            // Make sure process is not running.
            TerminateTargetProcess(exeName);

            // Get access token from ourselves.
            OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
            // Session ID is stored in the access token. For services it's normally 0.
            GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size);
            LogDebug("current session: %d, console session: %d", sessionId, WTSGetActiveConsoleSessionId());

            // We need to create a primary token for CreateProcessAsUser.
            if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
            {
                return perror("DuplicateTokenEx");
            }
            CloseHandle(currentProcess);

            sessionId = WTSGetActiveConsoleSessionId();
            // Change the session ID in the new access token to the target session ID.
            // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
            if (!SetTokenInformation(newToken, TokenSessionId, &sessionId, sizeof(sessionId)))
            {
                return perror("SetTokenInformation(TokenSessionId)");
            }

            LogInfo("Running process '%s' in session %d", cmdline, sessionId);
            // Create process with the new token.
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            // Don't forget to set the correct desktop.
            si.lpDesktop = L"WinSta0\\Winlogon";
            if (!CreateProcessAsUser(newToken, NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            {
                perror("CreateProcessAsUser");
            }
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
    WCHAR cmdline[MAX_PATH];
    WCHAR moduleName[CFG_MODULE_MAX];
    HANDLE workerHandle = 0;
    DWORD status;
    HANDLE sasDll;

    // Read the registry configuration.
    CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));
    status = CfgReadString(moduleName, REG_CONFIG_AUTOSTART_VALUE, cmdline, RTL_NUMBER_OF(cmdline), NULL);
    if (ERROR_SUCCESS != status)
    {
        perror("RegQueryValueEx(Autostart)");
        goto cleanup;
    }

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
    LogInfo("Starting worker thread");
    workerHandle = CreateThread(NULL, 0, WorkerThread, cmdline, 0, NULL);
    if (!workerHandle)
    {
        perror("CreateThread");
        goto cleanup;
    }

    // Signal the console change event to trigger initial spawning of the target process.
    SetEvent(g_ConsoleEvent);

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
    if (eventType < RTL_NUMBER_OF(g_SessionEventName))
        LogDebug("%s, session ID %d", g_SessionEventName[eventType], sn->dwSessionId);
    else
        LogDebug("<unknown event: %d>, session id %d", eventType, sn->dwSessionId);

    if (eventType == WTS_CONSOLE_CONNECT || eventType == WTS_SESSION_LOGON)
    {
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
