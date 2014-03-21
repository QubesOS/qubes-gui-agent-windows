#include <windows.h>

#include "log.h"

#define SERVICE_NAME TEXT("QTWHelper")
// Value below should contain path to the executable to launch at system start.
#define CONFIG_REG_KEY TEXT("Software\\Invisible Things Lab\\Qubes Tools")
#define CONFIG_REG_AUTOSTART_VALUE TEXT("Autostart")
#define CONFIG_REG_LOG_VALUE TEXT("LogDir")

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;

void WINAPI ServiceMain(DWORD argc, TCHAR *argv[]);
DWORD WINAPI ControlHandlerEx(DWORD controlCode, DWORD eventType, void *eventData, void *context);

// Entry point.
int main(int argc, TCHAR *argv[])
{
    SERVICE_TABLE_ENTRY	serviceTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    StartServiceCtrlDispatcher(serviceTable);
}

DWORD SpawnProcess(TCHAR *cmdline)
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD sessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    // Get access token from ourselves.
    OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size);
    logf("current session: %d, console session: %d", sessionId,  WTSGetActiveConsoleSessionId());

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

    logf("Running process '%s' in session %d", cmdline, sessionId);
    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // Don't forget to set the correct desktop.
    si.lpDesktop = TEXT("WinSta0\\Winlogon");
    if (!CreateProcessAsUser(newToken, NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        perror("CreateProcessAsUser1");
    }

    logf("done");
    return ERROR_SUCCESS;
}

void WINAPI ServiceMain(DWORD argc, TCHAR *argv[])
{
    TCHAR cmdline[MAX_PATH];
    HANDLE workerHandle = 0;
    HKEY key = 0;
    DWORD size;
    DWORD type;
    DWORD result;
    TCHAR logPath[MAX_PATH];

    // Read the registry configuration.
    if (ERROR_SUCCESS != RegOpenKey(HKEY_LOCAL_MACHINE, CONFIG_REG_KEY, &key))
    {
        log_init(TEXT("c:\\"), SERVICE_NAME);
        errorf("Opening config key '%s' failed, exiting", CONFIG_REG_KEY);
        goto cleanup;
    }

    RtlZeroMemory(logPath, sizeof(logPath));
    size = sizeof(logPath) - sizeof(TCHAR);
    result = RegQueryValueEx(key, CONFIG_REG_LOG_VALUE, NULL, &type, (BYTE*)logPath, &size);
    if (ERROR_SUCCESS != result)
    {
        log_init(TEXT("c:\\"), SERVICE_NAME);
        SetLastError(result);
        perror("RegQueryValueEx(LogPath)");
        // don't fail
    }

    log_init(logPath, SERVICE_NAME);

    if (type != REG_SZ)
    {
        log_init(TEXT("c:\\"), SERVICE_NAME);
        errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", CONFIG_REG_LOG_VALUE, type);
        // don't fail
    }

    size = sizeof(cmdline);
    result = RegQueryValueEx(key, CONFIG_REG_AUTOSTART_VALUE, NULL, &type, (BYTE*)cmdline, &size);
    if (ERROR_SUCCESS != result)
    {
        SetLastError(result);
        perror("RegQueryValueEx(Autostart)");
        goto cleanup;
    }

    if (type != REG_SZ)
    {
        errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", CONFIG_REG_LOG_VALUE, type);
        goto cleanup;
    }

    g_Status.dwServiceType        = SERVICE_WIN32;
    g_Status.dwCurrentState       = SERVICE_START_PENDING;
    g_Status.dwControlsAccepted   = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_Status.dwWin32ExitCode      = 0;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint         = 0;
    g_Status.dwWaitHint           = 0;
    g_StatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, ControlHandlerEx, NULL);
    if (g_StatusHandle == 0)
    {
        perror("RegisterServiceCtrlHandlerEx");
        goto cleanup;
    }

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);


    // Start the process.
    SpawnProcess(cmdline);

cleanup:
    if (key)
        RegCloseKey(key);
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = GetLastError();
    SetServiceStatus(g_StatusHandle, &g_Status);

    logf("end");
    return;
}

DWORD WINAPI ControlHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    switch(dwControl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOPPED;
        logf("stopping...");
        SetServiceStatus(g_StatusHandle, &g_Status);
        break;

    default:
        logf("ControlHandlerEx: code 0x%x, event 0x%x", dwControl, dwEventType);
        break;
    }

    return ERROR_SUCCESS;
}
