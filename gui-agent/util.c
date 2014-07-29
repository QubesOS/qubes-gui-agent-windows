#define OEMRESOURCE
#include <windows.h>
#include <aclapi.h>

#include "main.h"
#include "resource.h"

#include "log.h"

HANDLE CreateNamedEvent(WCHAR *name)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    EXPLICIT_ACCESS ea = { 0 };
    PACL acl = NULL;
    HANDLE event = NULL;

    // we're running as SYSTEM at the start, default ACL for new objects is too restrictive
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfAccessPermissions = EVENT_MODIFY_STATE | READ_CONTROL;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.ptstrName = L"EVERYONE";

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

ULONG IncreaseProcessWorkingSetSize(SIZE_T uNewMinimumWorkingSetSize, SIZE_T uNewMaximumWorkingSetSize)
{
    SIZE_T uMinimumWorkingSetSize = 0;
    SIZE_T uMaximumWorkingSetSize = 0;

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize))
        return perror("GetProcessWorkingSetSize");

    if (!SetProcessWorkingSetSize(GetCurrentProcess(), uNewMinimumWorkingSetSize, uNewMaximumWorkingSetSize))
        return perror("SetProcessWorkingSetSize");

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &uMinimumWorkingSetSize, &uMaximumWorkingSetSize))
        return perror("GetProcessWorkingSetSize");

    LogDebug("New working set size: %d pages\n", uMaximumWorkingSetSize >> 12);

    return ERROR_SUCCESS;
}

ULONG HideCursors(void)
{
    HCURSOR	hBlankCursor;
    HCURSOR	hBlankCursorCopy;
    UCHAR i;
    ULONG CursorsToHide[] = {
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

    LogDebug("start");
    hBlankCursor = LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_BLANK), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE);
    if (!hBlankCursor)
        return perror("LoadImage");

    for (i = 0; i < RTL_NUMBER_OF(CursorsToHide); i++)
    {
        // The system destroys hcur by calling the DestroyCursor function.
        // Therefore, hcur cannot be a cursor loaded using the LoadCursor function.
        // To specify a cursor loaded from a resource, copy the cursor using
        // the CopyCursor function, then pass the copy to SetSystemCursor.
        hBlankCursorCopy = CopyCursor(hBlankCursor);
        if (!hBlankCursorCopy)
            return perror("CopyCursor");

        if (!SetSystemCursor(hBlankCursorCopy, CursorsToHide[i]))
            return perror("SetSystemCursor");
    }

    if (!DestroyCursor(hBlankCursor))
        return perror("DestroyCursor");

    return ERROR_SUCCESS;
}

ULONG DisableEffects(void)
{
    ANIMATIONINFO AnimationInfo;

    LogDebug("start");
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)FALSE, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETDROPSHADOW)");

    AnimationInfo.cbSize = sizeof(AnimationInfo);
    AnimationInfo.iMinAnimate = FALSE;

    if (!SystemParametersInfo(SPI_SETANIMATION, sizeof(AnimationInfo), &AnimationInfo, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETANIMATION)");

    return ERROR_SUCCESS;
}

// Set current thread's desktop to the current input desktop.
ULONG AttachToInputDesktop(void)
{
    ULONG uResult = ERROR_SUCCESS;
    HDESK desktop = 0, oldDesktop = 0;
    WCHAR name[256];
    DWORD needed;
    DWORD sessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    LogVerbose("start");
    desktop = OpenInputDesktop(0, FALSE,
        DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL
        | DESKTOP_JOURNALPLAYBACK | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);

    if (!desktop)
    {
        uResult = perror("OpenInputDesktop");
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
        uResult = perror("SetThreadDesktop");
        goto cleanup;
    }

    g_DesktopHwnd = GetDesktopWindow();

cleanup:
    if (oldDesktop)
    if (!CloseDesktop(oldDesktop))
        perror("CloseDesktop(previous)");
    return uResult;
}

// Convert memory page number in the screen buffer to a rectangle that covers it.
void PageToRect(ULONG uPageNumber, OUT PRECT pRect)
{
    ULONG uStride = g_ScreenWidth * 4;
    ULONG uPageStart = uPageNumber * PAGE_SIZE;

    pRect->left = (uPageStart % uStride) / 4;
    pRect->top = uPageStart / uStride;
    pRect->right = ((uPageStart + PAGE_SIZE - 1) % uStride) / 4;
    pRect->bottom = (uPageStart + PAGE_SIZE - 1) / uStride;

    if (pRect->left > pRect->right) // page crossed right border
    {
        pRect->left = 0;
        pRect->right = g_ScreenWidth - 1;
    }
}
