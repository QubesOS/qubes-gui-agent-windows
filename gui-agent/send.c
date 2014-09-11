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
static ULONG PrepareShmCmd(PWATCHED_DC pWatchedDC, struct shm_cmd **ppShmCmd)
{
    QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
    ULONG uResult;
    ULONG uShmCmdSize = 0;
    struct shm_cmd *pShmCmd = NULL;
    PPFN_ARRAY	pPfnArray = NULL;
    HWND	hWnd = 0;
    ULONG	uWidth;
    ULONG	uHeight;
    ULONG	ulBitCount;
    BOOL	bIsScreen;
    ULONG	i;

    if (!ppShmCmd)
        return ERROR_INVALID_PARAMETER;

    *ppShmCmd = NULL;
    //debugf("start");

    if (!pWatchedDC)
    {
        LogDebug("fullcreen capture");

        pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pPfnArray->uNumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        uResult = GetWindowData(0, &QvGetSurfaceDataResponse, pPfnArray);
        if (ERROR_SUCCESS != uResult)
        {
            LogError("GetWindowData() failed with error %d\n", uResult);
            return uResult;
        }

        uWidth = QvGetSurfaceDataResponse.uWidth;
        uHeight = QvGetSurfaceDataResponse.uHeight;
        ulBitCount = QvGetSurfaceDataResponse.ulBitCount;

        bIsScreen = TRUE;
    }
    else
    {
        hWnd = pWatchedDC->hWnd;

        uWidth = pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left;
        uHeight = pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top;
        ulBitCount = 32;

        bIsScreen = FALSE;

        pPfnArray = pWatchedDC->pPfnArray;
    }

    LogDebug("Window %dx%d %d bpp at (%d,%d), fullscreen: %d\n", uWidth, uHeight, ulBitCount,
        pWatchedDC ? pWatchedDC->rcWindow.left : 0,
        pWatchedDC ? pWatchedDC->rcWindow.top : 0, bIsScreen);

    LogVerbose("PFNs: %d; 0x%x, 0x%x, 0x%x\n", pPfnArray->uNumberOf4kPages,
        pPfnArray->Pfn[0], pPfnArray->Pfn[1], pPfnArray->Pfn[2]);

    uShmCmdSize = sizeof(struct shm_cmd) + pPfnArray->uNumberOf4kPages * sizeof(uint32_t);

    pShmCmd = (struct shm_cmd*) malloc(uShmCmdSize);
    if (!pShmCmd)
    {
        LogError("Failed to allocate %d bytes for shm_cmd for window 0x%x\n", uShmCmdSize, hWnd);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    pShmCmd->shmid = 0;
    pShmCmd->width = uWidth;
    pShmCmd->height = uHeight;
    pShmCmd->bpp = ulBitCount;
    pShmCmd->off = 0;
    pShmCmd->num_mfn = pPfnArray->uNumberOf4kPages;
    pShmCmd->domid = 0;

    for (i = 0; i < pPfnArray->uNumberOf4kPages; i++)
        pShmCmd->mfns[i] = (uint32_t)pPfnArray->Pfn[i];

    *ppShmCmd = pShmCmd;

    if (!pWatchedDC)
        free(pPfnArray);

    //debugf("success");
    return ERROR_SUCCESS;
}

void send_pixmap_mfns(PWATCHED_DC pWatchedDC)
{
    ULONG uResult;
    struct shm_cmd *pShmCmd = NULL;
    struct msg_hdr hdr;
    int size;
    HWND hWnd = 0;

    LogVerbose("start");
    if (pWatchedDC)
        hWnd = pWatchedDC->hWnd;

    uResult = PrepareShmCmd(pWatchedDC, &pShmCmd);
    if (ERROR_SUCCESS != uResult)
    {
        LogError("PrepareShmCmd() failed with error %d", uResult);
        return;
    }

    if (pShmCmd->num_mfn == 0 || pShmCmd->num_mfn > MAX_MFN_COUNT)
    {
        LogError("too large num_mfn=%lu for window 0x%x", pShmCmd->num_mfn, (int)hWnd);
        free(pShmCmd);
        return;
    }

    size = pShmCmd->num_mfn * sizeof(uint32_t);

    hdr.type = MSG_MFNDUMP;
    hdr.window = (uint32_t)hWnd;
    hdr.untrusted_len = sizeof(struct shm_cmd) + size;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND(hdr);
    VchanSendBuffer(pShmCmd, sizeof(struct shm_cmd) + size);
    LeaveCriticalSection(&g_VchanCriticalSection);

    free(pShmCmd);
}

ULONG send_window_create(PWATCHED_DC pWatchedDC)
{
    WINDOWINFO wi;
    struct msg_hdr hdr;
    struct msg_create mc;
    PPFN_ARRAY pPfnArray = NULL;

    wi.cbSize = sizeof(wi);
    /* special case for full screen */
    if (pWatchedDC == NULL)
    {
        QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
        ULONG uResult;

        LogDebug("fullscreen");
        /* TODO: multiple screens? */
        wi.rcWindow.left = 0;
        wi.rcWindow.top = 0;

        pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pPfnArray->uNumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        uResult = GetWindowData(NULL, &QvGetSurfaceDataResponse, pPfnArray);
        if (ERROR_SUCCESS != uResult)
        {
            LogError("GetWindowData() failed with error %d", uResult);
            return uResult;
        }

        wi.rcWindow.right = QvGetSurfaceDataResponse.uWidth;
        wi.rcWindow.bottom = QvGetSurfaceDataResponse.uHeight;

        free(pPfnArray);

        hdr.window = 0;
    }
    else
    {
        LogDebug("hwnd=0x%x, (%d,%d)-(%d,%d), override=%d", pWatchedDC->hWnd,
            pWatchedDC->rcWindow.left, pWatchedDC->rcWindow.top,
            pWatchedDC->rcWindow.right, pWatchedDC->rcWindow.bottom,
            pWatchedDC->bOverrideRedirect);

        hdr.window = (uint32_t)pWatchedDC->hWnd;
        wi.rcWindow = pWatchedDC->rcWindow;
    }

    hdr.type = MSG_CREATE;

    mc.x = wi.rcWindow.left;
    mc.y = wi.rcWindow.top;
    mc.width = wi.rcWindow.right - wi.rcWindow.left;
    mc.height = wi.rcWindow.bottom - wi.rcWindow.top;
    mc.parent = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
    mc.override_redirect = pWatchedDC ? pWatchedDC->bOverrideRedirect : FALSE;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, mc);
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (pWatchedDC && pWatchedDC->bVisible && !pWatchedDC->bIconic)
        send_window_map(pWatchedDC);

    if (pWatchedDC)
        send_window_hints(pWatchedDC->hWnd, PPosition); // program-specified position

    return ERROR_SUCCESS;
}

ULONG send_window_destroy(HWND hWnd)
{
    struct msg_hdr hdr;

    LogDebug("0x%x", hWnd);
    hdr.type = MSG_DESTROY;
    hdr.window = (uint32_t)hWnd;
    hdr.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND(hdr);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

ULONG send_window_flags(HWND hWnd, uint32_t flags_set, uint32_t flags_unset)
{
    struct msg_hdr hdr;
    struct msg_window_flags flags;

    LogDebug("0x%x: set 0x%x, unset 0x%x", hWnd, flags_set, flags_unset);
    hdr.type = MSG_WINDOW_FLAGS;
    hdr.window = (uint32_t)hWnd;
    hdr.untrusted_len = 0;
    flags.flags_set = flags_set;
    flags.flags_unset = flags_unset;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, flags);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

void send_window_hints(HWND hWnd, uint32_t flags)
{
    struct msg_hdr hdr;
    struct msg_window_hints msg = { 0 };

    msg.flags = flags;
    LogDebug("flags: 0x%lx", flags);

    hdr.window = (uint32_t)hWnd;
    hdr.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

void send_screen_hints(void)
{
    struct msg_hdr hdr;
    struct msg_window_hints msg = { 0 };

    msg.flags = PMinSize; // minimum size
    msg.min_width = MIN_RESOLUTION_WIDTH;
    msg.min_height = MIN_RESOLUTION_HEIGHT;
    LogDebug("min %dx%d", msg.min_width, msg.min_height);

    hdr.window = 0; // screen
    hdr.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

ULONG send_window_unmap(HWND hWnd)
{
    struct msg_hdr hdr;

    LogInfo("Unmapping window 0x%x\n", hWnd);

    hdr.type = MSG_UNMAP;
    hdr.window = (uint32_t)hWnd;
    hdr.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND(hdr);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

ULONG send_window_map(PWATCHED_DC pWatchedDC)
{
    struct msg_hdr hdr;
    struct msg_map_info mmi;

    if (pWatchedDC)
        LogInfo("Mapping window 0x%x\n", pWatchedDC->hWnd);
    else
        LogInfo("Mapping desktop window\n");

    hdr.type = MSG_MAP;
    if (pWatchedDC)
        hdr.window = (uint32_t)pWatchedDC->hWnd;
    else
        hdr.window = 0;
    hdr.untrusted_len = 0;

    if (pWatchedDC && pWatchedDC->ModalParent)
        mmi.transient_for = (uint32_t)pWatchedDC->ModalParent;
    else
        mmi.transient_for = (uint32_t)INVALID_HANDLE_VALUE;

    if (pWatchedDC)
        mmi.override_redirect = pWatchedDC->bOverrideRedirect;
    else
        mmi.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, mmi);
    LeaveCriticalSection(&g_VchanCriticalSection);

    // if the window takes the whole screen (like logon window), try to make it fullscreen in dom0
    if (!pWatchedDC ||
        (pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left == g_ScreenWidth &&
        pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top == g_ScreenHeight))
    {
        send_screen_hints(); // min/max screen size
        send_wmname(NULL);
        if (g_ScreenWidth == g_HostScreenWidth && g_ScreenHeight == g_HostScreenHeight)
        {
            LogDebug("fullscreen window");
            send_window_flags(pWatchedDC ? pWatchedDC->hWnd : NULL, WINDOW_FLAG_FULLSCREEN, 0);
        }
    }

    return ERROR_SUCCESS;
}

ULONG send_window_configure(PWATCHED_DC pWatchedDC)
{
    struct msg_hdr hdr;
    struct msg_configure mc;
    struct msg_map_info mmi;

    if (pWatchedDC)
    {
        LogDebug("0x%x", pWatchedDC->hWnd);
        hdr.window = (uint32_t)pWatchedDC->hWnd;

        hdr.type = MSG_CONFIGURE;

        mc.x = pWatchedDC->rcWindow.left;
        mc.y = pWatchedDC->rcWindow.top;
        mc.width = pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left;
        mc.height = pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top;
        mc.override_redirect = 0;
    }
    else // whole screen
    {
        LogDebug("fullscreen: (0,0) %dx%d", g_ScreenWidth, g_ScreenHeight);
        hdr.window = 0;

        hdr.type = MSG_CONFIGURE;

        mc.x = 0;
        mc.y = 0;
        mc.width = g_ScreenWidth;
        mc.height = g_ScreenHeight;
        mc.override_redirect = 0;
    }

    EnterCriticalSection(&g_VchanCriticalSection);
    /* don't send resize to 0x0 - this window is just hiding itself, MSG_UNMAP
    * will follow */
    if (mc.width > 0 && mc.height > 0)
    {
        VCHAN_SEND_MSG(hdr, mc);
    }

    if (pWatchedDC && pWatchedDC->bVisible)
    {
        mmi.transient_for = (uint32_t)INVALID_HANDLE_VALUE; /* TODO? */
        mmi.override_redirect = pWatchedDC->bOverrideRedirect;

        hdr.type = MSG_MAP;
        VCHAN_SEND_MSG(hdr, mmi);
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

// Send screen resolution back do gui daemon.
ULONG send_screen_configure(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    struct msg_hdr hdr;
    struct msg_configure mc;

    LogDebug("(%d,%d) %dx%d", x, y, width, height);
    hdr.window = 0; // 0 = screen

    hdr.type = MSG_CONFIGURE;

    mc.x = x;
    mc.y = y;
    mc.width = width;
    mc.height = height;
    mc.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, mc);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

void send_window_damage_event(HWND hWnd, int x, int y, int width, int height)
{
    struct msg_shmimage mx;
    struct msg_hdr hdr;

    LogVerbose("0x%x (%d,%d)-(%d,%d)", hWnd, x, y, x + width, y + height);
    hdr.type = MSG_SHMIMAGE;
    hdr.window = (uint32_t)hWnd;
    mx.x = x;
    mx.y = y;
    mx.width = width;
    mx.height = height;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, mx);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

void send_wmname(HWND hWnd)
{
    struct msg_hdr hdr;
    struct msg_wmname msg;

    if (hWnd)
    {
        // FIXME: this fails for non-ascii strings
        if (!GetWindowTextA(hWnd, msg.data, sizeof(msg.data)))
        {
            // ignore empty/non-readable captions
            return;
        }
    }
    else
    {
        StringCchPrintfA(msg.data, RTL_NUMBER_OF(msg.data), "%s (Windows Desktop)", g_DomainName);
    }
    LogDebug("0x%x %S", hWnd, msg.data);

    hdr.window = (uint32_t)hWnd;
    hdr.type = MSG_WMNAME;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND_MSG(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

void send_protocol_version(void)
{
    uint32_t version = QUBES_GUI_PROTOCOL_VERSION_WINDOWS;
    EnterCriticalSection(&g_VchanCriticalSection);
    VCHAN_SEND(version);
    LeaveCriticalSection(&g_VchanCriticalSection);
}
