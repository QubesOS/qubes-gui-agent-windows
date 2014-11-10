#include <windows.h>
#include <stdlib.h>

#include "common.h"
#include "send.h"
#include "main.h"
#include "qvcontrol.h"
#include "vchan.h"

#include "qubes-gui-protocol.h"
#include "log.h"

#include <strsafe.h>

// Get PFNs of hWnd Window from QVideo driver and prepare relevant shm_cmd struct.
// if watchedDC == NULL, get PFNs for the whole screen
static ULONG PrepareShmCmd(IN const WATCHED_DC *watchedDC OPTIONAL, OUT struct shm_cmd **shmCmd)
{
    QV_GET_SURFACE_DATA_RESPONSE surfaceData;
    ULONG status;
    ULONG shmCmdSize = 0;
    PFN_ARRAY *pfnArray = NULL;
    ULONG width;
    ULONG height;
    ULONG bpp;
    BOOL isScreen;
    ULONG i;

    if (!shmCmd)
        return ERROR_INVALID_PARAMETER;

    *shmCmd = NULL;
    //debugf("start");

    if (!watchedDC) // whole screen
    {
        LogDebug("fullcreen capture");

        pfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pfnArray->NumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        status = GetWindowData(NULL, &surfaceData, pfnArray);
        if (ERROR_SUCCESS != status)
        {
            LogError("GetWindowData() failed with error %d\n", status);
            return status;
        }

        width = surfaceData.Width;
        height = surfaceData.Height;
        bpp = surfaceData.Bpp;

        isScreen = TRUE;
    }
    else
    {
        width = watchedDC->WindowRect.right - watchedDC->WindowRect.left;
        height = watchedDC->WindowRect.bottom - watchedDC->WindowRect.top;
        bpp = 32;

        isScreen = FALSE;

        pfnArray = watchedDC->PfnArray;
    }

    LogDebug("Window %dx%d %d bpp at (%d,%d), fullscreen: %d\n", width, height, bpp,
        watchedDC ? watchedDC->WindowRect.left : 0,
        watchedDC ? watchedDC->WindowRect.top : 0, isScreen);

    LogVerbose("PFNs: %d; 0x%x, 0x%x, 0x%x\n", pfnArray->NumberOf4kPages,
        pfnArray->Pfn[0], pfnArray->Pfn[1], pfnArray->Pfn[2]);

    shmCmdSize = sizeof(struct shm_cmd) + pfnArray->NumberOf4kPages * sizeof(uint32_t);

    *shmCmd = (struct shm_cmd*) malloc(shmCmdSize);
    if (*shmCmd == NULL)
    {
        LogError("Failed to allocate %d bytes for shm_cmd for window 0x%x\n", shmCmdSize, watchedDC ? watchedDC->WindowHandle : NULL);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    (*shmCmd)->shmid = 0;
    (*shmCmd)->width = width;
    (*shmCmd)->height = height;
    (*shmCmd)->bpp = bpp;
    (*shmCmd)->off = 0;
    (*shmCmd)->num_mfn = pfnArray->NumberOf4kPages;
    (*shmCmd)->domid = 0;

    for (i = 0; i < pfnArray->NumberOf4kPages; i++)
        (*shmCmd)->mfns[i] = (uint32_t) pfnArray->Pfn[i];

    if (!watchedDC)
        free(pfnArray);

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG SendWindowMfns(IN const WATCHED_DC *watchedDC)
{
    ULONG status;
    struct shm_cmd *shmCmd = NULL;
    struct msg_hdr header;
    UINT32 size;
    HWND window = NULL;

    LogVerbose("start");
    if (watchedDC)
        window = watchedDC->WindowHandle;

    status = PrepareShmCmd(watchedDC, &shmCmd);
    if (ERROR_SUCCESS != status)
        return perror2(status, "PrepareShmCmd");

    if (shmCmd->num_mfn == 0 || shmCmd->num_mfn > MAX_MFN_COUNT)
    {
        LogError("too large num_mfn=%lu for window 0x%x", shmCmd->num_mfn, window);
        free(shmCmd);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    size = shmCmd->num_mfn * sizeof(uint32_t);

    header.type = MSG_MFNDUMP;
    header.window = (uint32_t) window;
    header.untrusted_len = sizeof(struct shm_cmd) + size;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND(header))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return perror2(ERROR_UNIDENTIFIED_ERROR, "VCHAN_SEND(header)");
    }

    status = VchanSendBuffer(shmCmd, sizeof(struct shm_cmd) + size);
    LeaveCriticalSection(&g_VchanCriticalSection);
    if (!status)
        return perror2(status, "VchanSendBuffer");

    free(shmCmd);

    return ERROR_SUCCESS;
}

ULONG SendWindowCreate(IN const WATCHED_DC *watchedDC)
{
    WINDOWINFO wi;
    struct msg_hdr header;
    struct msg_create createMsg;
    PFN_ARRAY *pfnArray = NULL;
    ULONG status;

    wi.cbSize = sizeof(wi);
    /* special case for full screen */
    if (watchedDC == NULL)
    {
        QV_GET_SURFACE_DATA_RESPONSE surfaceData;

        LogDebug("fullscreen");
        /* TODO: multiple screens? */
        wi.rcWindow.left = 0;
        wi.rcWindow.top = 0;

        pfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pfnArray->NumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        status = GetWindowData(NULL, &surfaceData, pfnArray);
        if (ERROR_SUCCESS != status)
            return perror2(status, "GetWindowData");

        wi.rcWindow.right = surfaceData.Width;
        wi.rcWindow.bottom = surfaceData.Height;

        free(pfnArray);

        header.window = 0;
    }
    else
    {
        LogDebug("hwnd=0x%x, (%d,%d)-(%d,%d), override=%d", watchedDC->WindowHandle,
            watchedDC->WindowRect.left, watchedDC->WindowRect.top,
            watchedDC->WindowRect.right, watchedDC->WindowRect.bottom,
            watchedDC->IsOverrideRedirect);

        header.window = (uint32_t) watchedDC->WindowHandle;
        wi.rcWindow = watchedDC->WindowRect;
    }

    header.type = MSG_CREATE;

    createMsg.x = wi.rcWindow.left;
    createMsg.y = wi.rcWindow.top;
    createMsg.width = wi.rcWindow.right - wi.rcWindow.left;
    createMsg.height = wi.rcWindow.bottom - wi.rcWindow.top;
    createMsg.parent = (uint32_t) INVALID_HANDLE_VALUE; /* TODO? */
    createMsg.override_redirect = watchedDC ? watchedDC->IsOverrideRedirect : FALSE;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND_MSG(header, createMsg))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (watchedDC && watchedDC->IsVisible && !watchedDC->IsIconic)
    {
        status = SendWindowMap(watchedDC);
        if (ERROR_SUCCESS != status)
            return status;
    }

    if (watchedDC)
    {
        status = SendWindowHints(watchedDC->WindowHandle, PPosition); // program-specified position
        if (ERROR_SUCCESS != status)
            return status;
    }

    return ERROR_SUCCESS;
}

ULONG SendWindowDestroy(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    LogDebug("0x%x", window);
    header.type = MSG_DESTROY;
    header.window = (uint32_t) window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(header);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset)
{
    struct msg_hdr header;
    struct msg_window_flags flags;
    BOOL status;

    LogDebug("0x%x: set 0x%x, unset 0x%x", window, flagsToSet, flagsToUnset);
    header.type = MSG_WINDOW_FLAGS;
    header.window = (uint32_t) window;
    header.untrusted_len = 0;
    flags.flags_set = flagsToSet;
    flags.flags_unset = flagsToUnset;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, flags);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowHints(IN HWND window, IN uint32_t flags)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    hintsMsg.flags = flags;
    LogDebug("flags: 0x%lx", flags);

    header.window = (uint32_t) window;
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, hintsMsg);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendScreenHints(void)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    hintsMsg.flags = PMinSize; // minimum size
    hintsMsg.min_width = MIN_RESOLUTION_WIDTH;
    hintsMsg.min_height = MIN_RESOLUTION_HEIGHT;
    LogDebug("min %dx%d", hintsMsg.min_width, hintsMsg.min_height);

    header.window = 0; // screen
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, hintsMsg);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowUnmap(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    LogInfo("Unmapping window 0x%x\n", window);

    header.type = MSG_UNMAP;
    header.window = (uint32_t) window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(header);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

// if watchedDC == 0, use the whole screen
ULONG SendWindowMap(IN const WATCHED_DC *watchedDC OPTIONAL)
{
    struct msg_hdr header;
    struct msg_map_info mapMsg;
    ULONG status;

    if (watchedDC)
        LogInfo("Mapping window 0x%x\n", watchedDC->WindowHandle);
    else
        LogInfo("Mapping desktop window\n");

    header.type = MSG_MAP;
    if (watchedDC)
        header.window = (uint32_t) watchedDC->WindowHandle;
    else
        header.window = 0;
    header.untrusted_len = 0;

    if (watchedDC && watchedDC->ModalParent)
        mapMsg.transient_for = (uint32_t) watchedDC->ModalParent;
    else
        mapMsg.transient_for = (uint32_t) INVALID_HANDLE_VALUE;

    if (watchedDC)
        mapMsg.override_redirect = watchedDC->IsOverrideRedirect;
    else
        mapMsg.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!VCHAN_SEND_MSG(header, mapMsg))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    // if the window takes the whole screen (like logon window), try to make it fullscreen in dom0
    if (!watchedDC ||
        (watchedDC->WindowRect.right - watchedDC->WindowRect.left == g_ScreenWidth &&
        watchedDC->WindowRect.bottom - watchedDC->WindowRect.top == g_ScreenHeight))
    {
        status = SendScreenHints(); // min/max screen size
        if (ERROR_SUCCESS != status)
            return status;

        status = SendWindowName(NULL, NULL); // desktop
        if (ERROR_SUCCESS != status)
            return status;

        if (g_ScreenWidth == g_HostScreenWidth && g_ScreenHeight == g_HostScreenHeight)
        {
            LogDebug("fullscreen window");
            status = SendWindowFlags(watchedDC ? watchedDC->WindowHandle : NULL, WINDOW_FLAG_FULLSCREEN, 0);
            if (ERROR_SUCCESS != status)
                return status;
        }
    }

    return ERROR_SUCCESS;
}

// if watchedDC == 0, use the whole screen
ULONG SendWindowConfigure(IN const WATCHED_DC *watchedDC OPTIONAL)
{
    struct msg_hdr header;
    struct msg_configure configureMsg;
    struct msg_map_info mapMsg;
    BOOL status;

    if (watchedDC)
    {
        LogDebug("0x%x", watchedDC->WindowHandle);
        header.window = (uint32_t) watchedDC->WindowHandle;

        header.type = MSG_CONFIGURE;

        configureMsg.x = watchedDC->WindowRect.left;
        configureMsg.y = watchedDC->WindowRect.top;
        configureMsg.width = watchedDC->WindowRect.right - watchedDC->WindowRect.left;
        configureMsg.height = watchedDC->WindowRect.bottom - watchedDC->WindowRect.top;
        configureMsg.override_redirect = 0;
    }
    else // whole screen
    {
        LogDebug("fullscreen: (0,0) %dx%d", g_ScreenWidth, g_ScreenHeight);
        header.window = 0;

        header.type = MSG_CONFIGURE;

        configureMsg.x = 0;
        configureMsg.y = 0;
        configureMsg.width = g_ScreenWidth;
        configureMsg.height = g_ScreenHeight;
        configureMsg.override_redirect = 0;
    }

    status = ERROR_SUCCESS;
    EnterCriticalSection(&g_VchanCriticalSection);
    /* don't send resize to 0x0 - this window is just hiding itself, MSG_UNMAP
    * will follow */
    if (configureMsg.width > 0 && configureMsg.height > 0)
    {
        status = VCHAN_SEND_MSG(header, configureMsg);
        if (!status)
            goto cleanup;
    }

    if (watchedDC && watchedDC->IsVisible)
    {
        mapMsg.transient_for = (uint32_t) INVALID_HANDLE_VALUE; /* TODO? */
        mapMsg.override_redirect = watchedDC->IsOverrideRedirect;

        header.type = MSG_MAP;
        status = VCHAN_SEND_MSG(header, mapMsg);
    }
cleanup:
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

// Send screen resolution back to gui daemon.
ULONG SendScreenConfigure(IN UINT32 x, IN UINT32 y, IN UINT32 width, IN UINT32 height)
{
    struct msg_hdr header;
    struct msg_configure configMsg;
    BOOL status;

    LogDebug("(%d,%d) %dx%d", x, y, width, height);
    header.window = 0; // 0 = screen

    header.type = MSG_CONFIGURE;

    configMsg.x = x;
    configMsg.y = y;
    configMsg.width = width;
    configMsg.height = height;
    configMsg.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, configMsg);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height)
{
    struct msg_shmimage shmMsg;
    struct msg_hdr header;
    BOOL status;

    LogVerbose("0x%x (%d,%d)-(%d,%d)", window, x, y, x + width, y + height);
    header.type = MSG_SHMIMAGE;
    header.window = (uint32_t) window;
    shmMsg.x = x;
    shmMsg.y = y;
    shmMsg.width = width;
    shmMsg.height = height;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, shmMsg);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL)
{
    struct msg_hdr header;
    struct msg_wmname nameMsg;
    BOOL status;

    if (window)
    {
        if (caption)
        {
            StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%S", caption);
        }
        else
        {
            if (0 == GetWindowTextA(window, nameMsg.data, RTL_NUMBER_OF(nameMsg.data)))
            {
                perror("GetWindowTextA");
                return ERROR_SUCCESS; // whatever
            }
        }
    }
    else
    {
        StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%s (Windows Desktop)", g_DomainName);
    }

    LogDebug("0x%x %S", window, nameMsg.data);

    header.window = (uint32_t) window;
    header.type = MSG_WMNAME;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, nameMsg);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendProtocolVersion(void)
{
    uint32_t version = QUBES_GUI_PROTOCOL_VERSION_WINDOWS;
    BOOL status;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND(version);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}
