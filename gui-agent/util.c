#define OEMRESOURCE
#include <windows.h>
#include <aclapi.h>
#include <strsafe.h>

#include "main.h"
#include "resource.h"

#include "log.h"

DWORD g_DisableCursor = TRUE;

static SID *BuildSid(void)
{
    SID_IDENTIFIER_AUTHORITY sia = SECURITY_NT_AUTHORITY; // don't use LOCAL - only processes running in interactive session belong to that...
    SID *sid = NULL;
    if (!AllocateAndInitializeSid(&sia, 1, SECURITY_AUTHENTICATED_USER_RID, 0, 0, 0, 0, 0, 0, 0, &sid))
    {
        perror("AllocateAndInitializeSid");
    }
    return sid;
}

// returns NULL on failure
HANDLE CreateNamedEvent(IN const WCHAR *name)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    EXPLICIT_ACCESS ea = { 0 };
    ACL *acl = NULL;
    HANDLE event = NULL;
    SID *localSid = NULL;

    LogDebug("%s", name);

    localSid = BuildSid();

    // we're running as SYSTEM at the start, default ACL for new objects is too restrictive
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfAccessPermissions = EVENT_MODIFY_STATE | READ_CONTROL | SYNCHRONIZE;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = (WCHAR *) localSid;

    if (SetEntriesInAcl(1, &ea, NULL, &acl) != ERROR_SUCCESS)
    {
        perror("SetEntriesInAcl");
        goto cleanup;
    }
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
    {
        perror("InitializeSecurityDescriptor");
        goto cleanup;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE))
    {
        perror("SetSecurityDescriptorDacl");
        goto cleanup;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // autoreset, not signaled
    event = CreateEvent(&sa, FALSE, FALSE, name);

    if (!event)
    {
        perror("CreateEvent");
        goto cleanup;
    }

cleanup:
    if (acl)
        LocalFree(acl);
    return event;
}

ULONG StartProcess(IN const WCHAR *executable, OUT HANDLE *processHandle)
{
    STARTUPINFO si = { 0 };
    PROCESS_INFORMATION pi;
    WCHAR exePath[MAX_PATH]; // cmdline can't be read-only

    LogDebug("%s", executable);

    StringCchCopy(exePath, RTL_NUMBER_OF(exePath), executable);

    si.cb = sizeof(si);
    //si.wShowWindow = SW_HIDE;
    //si.dwFlags = STARTF_USESHOWWINDOW;
    if (!CreateProcess(NULL, exePath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return perror("CreateProcess");
    CloseHandle(pi.hThread);
    *processHandle = pi.hProcess;
    return ERROR_SUCCESS;
}

ULONG IncreaseProcessWorkingSetSize(IN SIZE_T minimumSize, IN SIZE_T maximumSize)
{
    if (!SetProcessWorkingSetSize(GetCurrentProcess(), minimumSize, maximumSize))
        return perror("SetProcessWorkingSetSize");

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &minimumSize, &maximumSize))
        return perror("GetProcessWorkingSetSize");

    LogDebug("New working set size: %d pages\n", maximumSize >> 12);

    return ERROR_SUCCESS;
}

ULONG HideCursors(void)
{
    HCURSOR	blankCursor;
    HCURSOR	blankCursorCopy;
    UCHAR i;
    ULONG cursorsToHide[] = {
        OCR_APPSTARTING,	// Standard arrow and small hourglass
        OCR_NORMAL,		// Standard arrow
        OCR_CROSS,		// Crosshair
        OCR_HAND,		// Hand
        OCR_IBEAM,		// I-beam
        OCR_NO,			// Slashed circle
        OCR_SIZEALL,		// Four-pointed arrow pointing north, south, east, and west
        OCR_SIZENESW,		// Double-pointed arrow pointing northeast and southwest
        OCR_SIZENS,		// Double-pointed arrow pointing north and south
        OCR_SIZENWSE,		// Double-pointed arrow pointing northwest and southeast
        OCR_SIZEWE,		// Double-pointed arrow pointing west and east
        OCR_UP,			// Vertical arrow
        OCR_WAIT		// Hourglass
    };

    LogVerbose("start");

    if (!g_DisableCursor)
        return ERROR_SUCCESS;

    LogDebug("disabling cursors");

    blankCursor = LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_BLANK), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE);
    if (!blankCursor)
        return perror("LoadImage");

    for (i = 0; i < RTL_NUMBER_OF(cursorsToHide); i++)
    {
        // The system destroys hcur by calling the DestroyCursor function.
        // Therefore, hcur cannot be a cursor loaded using the LoadCursor function.
        // To specify a cursor loaded from a resource, copy the cursor using
        // the CopyCursor function, then pass the copy to SetSystemCursor.
        blankCursorCopy = CopyCursor(blankCursor);
        if (!blankCursorCopy)
            return perror("CopyCursor");

        if (!SetSystemCursor(blankCursorCopy, cursorsToHide[i]))
            return perror("SetSystemCursor");
    }

    if (!DestroyCursor(blankCursor))
        return perror("DestroyCursor");

    return ERROR_SUCCESS;
}

ULONG DisableEffects(void)
{
    ANIMATIONINFO animationInfo;

    LogDebug("start");
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (void *) FALSE, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETDROPSHADOW)");

    animationInfo.cbSize = sizeof(animationInfo);
    animationInfo.iMinAnimate = FALSE;

    if (!SystemParametersInfo(SPI_SETANIMATION, sizeof(animationInfo), &animationInfo, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETANIMATION)");

    return ERROR_SUCCESS;
}

// Set current thread's desktop to the current input desktop.
ULONG AttachToInputDesktop(void)
{
    ULONG status = ERROR_SUCCESS;
    HDESK desktop = 0, oldDesktop = 0;
    HANDLE currentProcess = GetCurrentProcess();
#ifdef DEBUG
    HANDLE currentToken;
    DWORD sessionId;
    DWORD size;
    WCHAR name[256];
    DWORD needed;
#endif

    LogVerbose("start");
    desktop = OpenInputDesktop(0, FALSE,
        DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL
        | DESKTOP_JOURNALPLAYBACK | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);

    if (!desktop)
    {
        status = perror("OpenInputDesktop");
        goto cleanup;
    }

#ifdef DEBUG
    if (!GetUserObjectInformation(desktop, UOI_NAME, name, sizeof(name), &needed))
    {
        perror("GetUserObjectInformation");
    }
    else
    {
        // Get access token from ourselves.
        OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
        // Session ID is stored in the access token.
        GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size);
        CloseHandle(currentToken);
        LogDebug("current input desktop: %s, current session: %d, console session: %d",
            name, sessionId, WTSGetActiveConsoleSessionId());
    }
#endif

    // Close old handle to prevent object leaks.
    oldDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop(desktop))
    {
        status = perror("SetThreadDesktop");
        goto cleanup;
    }

    g_DesktopWindow = GetDesktopWindow();

cleanup:
    if (oldDesktop)
        if (!CloseDesktop(oldDesktop))
            perror("CloseDesktop(previous)");
    return status;
}

// Convert memory page number in the screen buffer to a rectangle that covers it.
void PageToRect(IN ULONG pageNumber, OUT RECT *rect)
{
    ULONG stride = g_ScreenWidth * 4;
    ULONG pageStart = pageNumber * PAGE_SIZE;

    rect->left = (pageStart % stride) / 4;
    rect->top = pageStart / stride;
    rect->right = ((pageStart + PAGE_SIZE - 1) % stride) / 4;
    rect->bottom = (pageStart + PAGE_SIZE - 1) / stride;

    if (rect->left > rect->right) // page crossed right border
    {
        rect->left = 0;
        rect->right = g_ScreenWidth - 1;
    }
}
