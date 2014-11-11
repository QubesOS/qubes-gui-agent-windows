#define OEMRESOURCE
#include <windows.h>
#include <aclapi.h>
#include <strsafe.h>

#include "main.h"
#include "resource.h"

#include "log.h"

DWORD g_DisableCursor = TRUE;

// SID for all authenticated users
static SID *BuildLocalSid(void)
{
    SID_IDENTIFIER_AUTHORITY sia = SECURITY_NT_AUTHORITY; // don't use LOCAL - only processes running in interactive session belong to that...
    SID *sid = NULL;
    if (!AllocateAndInitializeSid(&sia, 1, SECURITY_AUTHENTICATED_USER_RID, 0, 0, 0, 0, 0, 0, 0, &sid))
    {
        perror("AllocateAndInitializeSid");
    }
    return sid;
}

// Create ACL that grants specified access to all authenticated users.
// On success caller needs to LocalFree() sa->lpSecurityDescriptor->Dacl,
// sa->lpSecurityDescriptor and sa. Security API is a real pain.
static ULONG CreatePublicAcl(IN DWORD accessMask, OUT SECURITY_ATTRIBUTES **sa)
{
    SECURITY_DESCRIPTOR *sd = NULL;
    ACL *acl = NULL;
    EXPLICIT_ACCESS ea = { 0 }; // this one can be a local variable.
    ULONG status = ERROR_UNIDENTIFIED_ERROR;

    ea.grfAccessMode = SET_ACCESS;
    ea.grfAccessPermissions = accessMask;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = (WCHAR *) BuildLocalSid();

    status = SetEntriesInAcl(1, &ea, NULL, &acl);
    if (status != ERROR_SUCCESS)
    {
        perror2(status, "SetEntriesInAcl");
        goto cleanup;
    }

    sd = (SECURITY_DESCRIPTOR *) LocalAlloc(LMEM_ZEROINIT, sizeof(SECURITY_DESCRIPTOR));
    if (!sd)
    {
        status = perror("LocalAlloc");
        goto cleanup;
    }

    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
    {
        status = perror("InitializeSecurityDescriptor");
        goto cleanup;
    }

    if (!SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE))
    {
        perror("SetSecurityDescriptorDacl");
        goto cleanup;
    }

    *sa = (SECURITY_ATTRIBUTES *) LocalAlloc(LMEM_ZEROINIT, sizeof(SECURITY_ATTRIBUTES));

    (*sa)->nLength = sizeof(SECURITY_ATTRIBUTES);
    (*sa)->lpSecurityDescriptor = sd;
    (*sa)->bInheritHandle = FALSE;

    status = ERROR_SUCCESS;

cleanup:
    if (ea.Trustee.ptstrName)
        FreeSid((SID *) ea.Trustee.ptstrName);
    if (ERROR_SUCCESS != status)
    {
        if (acl)
            LocalFree(acl);
        if (sd)
            LocalFree(sd);
        if (*sa)
            LocalFree(*sa);
    }
    return status;
}

// returns NULL on failure
HANDLE CreateNamedEvent(IN const WCHAR *name)
{
    HANDLE event = NULL;
    SECURITY_ATTRIBUTES *sa;
    ULONG status;

    LogDebug("%s", name);

    status = CreatePublicAcl(EVENT_MODIFY_STATE | READ_CONTROL | SYNCHRONIZE, &sa);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "CreatePublicAcl");
        return NULL;
    }

    // autoreset, not signaled
    event = CreateEvent(sa, FALSE, FALSE, name);
    status = GetLastError();

    // Just reiterating that security API is amazing...
    LocalFree(((SECURITY_DESCRIPTOR *) sa->lpSecurityDescriptor)->Dacl);
    LocalFree(sa->lpSecurityDescriptor);
    LocalFree(sa);

    if (!event)
        perror2(status, "CreateEvent");

    return event;
}

// returns NULL on failure
HANDLE CreateNamedMailslot(IN const WCHAR *name)
{
    HANDLE slot = NULL;
    SECURITY_ATTRIBUTES *sa;
    ULONG status;

    LogDebug("%s", name);

    status = CreatePublicAcl(GENERIC_READ | GENERIC_WRITE | READ_CONTROL | SYNCHRONIZE, &sa);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "CreatePublicAcl");
        return NULL;
    }

    slot = CreateMailslot(name, 0, MAILSLOT_WAIT_FOREVER, sa);
    status = GetLastError();

    // Just reiterating that security API is amazing...
    LocalFree(((SECURITY_DESCRIPTOR *) sa->lpSecurityDescriptor)->Dacl);
    LocalFree(sa->lpSecurityDescriptor);
    LocalFree(sa);

    if (!slot)
        perror2(status, "CreateMailslot");

    return slot;
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
