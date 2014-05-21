#define OEMRESOURCE
#include <windows.h>
#include <aclapi.h>
#include <winsock2.h>
#include "tchar.h"
#include "qubes-gui-protocol.h"
#include "libvchan.h"
#include "glue.h"
#include "qvcontrol.h"
#include "shell_events.h"
#include "resource.h"
#include "log.h"

// window hints constants
// http://tronche.com/gui/x/icccm/sec-4.html
#define USPosition  1       // User-specified x, y
#define USSize      2       // User-specified width, height
#define PPosition   4       // Program-specified position
#define PSize       8       // Program-specified size
#define PMinSize    16      // Program-specified minimum size
#define PMaxSize    32      // Program-specified maximum size
#define PResizeInc  64      // Program-specified resize increments
#define PAspect     128 	// Program-specified min and max aspect ratios
#define PBaseSize   256 	// Program-specified base size
#define PWinGravity 512 	// Program-specified window gravity

#define FULLSCREEN_ON_EVENT_NAME L"WGA_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"WGA_FULLSCREEN_OFF"

CRITICAL_SECTION g_VchanCriticalSection;

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

BOOL g_bVchanClientConnected = FALSE;
BOOL g_bFullScreenMode = FALSE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

// Signal to change resolution to g_ResolutionChangeParams.
// This is pretty ugly but we need to trigger resolution change from a separate thread...
HANDLE g_ResolutionChangeEvent = NULL;

#define RESOLUTION_CHANGE_TIMEOUT 500

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
// This thread is created in handle_configure().
HANDLE g_ResolutionChangeThread = NULL;

// Event signaled by handle_configure on receiving screen resize message.
// g_ResolutionChangeThread handles that and throttles too frequent resolution changes.
HANDLE g_ResolutionChangeRequestedEvent = NULL;

struct RESOLUTION_CHANGE_PARAMS
{
    LONG width;
    LONG height;
    LONG x; // this is needed to send ACK to daemon although it's useless for fullscreen
    LONG y;
};

struct RESOLUTION_CHANGE_PARAMS g_ResolutionChangeParams = {0};

char g_HostName[256] = "<unknown>";

ULONG HideCursors();
ULONG DisableEffects();

HANDLE CreateNamedEvent(WCHAR *name)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    EXPLICIT_ACCESS ea = {0};
    PACL acl = NULL;
    HANDLE event = NULL;

    // we're running as SYSTEM at the start, default ACL for new objects is too restrictive
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfAccessPermissions = EVENT_MODIFY_STATE|READ_CONTROL;
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

/* Get PFNs of hWnd Window from QVideo driver and prepare relevant shm_cmd
 * struct.
 */
ULONG PrepareShmCmd(
    PWATCHED_DC pWatchedDC,
    struct shm_cmd **ppShmCmd
    )
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
        debugf("fullcreen capture");
        // fullscreen capture

        pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pPfnArray->uNumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        uResult = GetWindowData(0, &QvGetSurfaceDataResponse, pPfnArray);
        if (ERROR_SUCCESS != uResult)
        {
            errorf("GetWindowData() failed with error %d\n", uResult);
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

    logf("Window %dx%d %d bpp at (%d,%d), fullscreen: %d\n", uWidth, uHeight, ulBitCount,
        pWatchedDC ? pWatchedDC->rcWindow.left : 0,
        pWatchedDC ? pWatchedDC->rcWindow.top : 0, bIsScreen);

    logf("PFNs: %d; 0x%x, 0x%x, 0x%x\n", pPfnArray->uNumberOf4kPages,
        pPfnArray->Pfn[0], pPfnArray->Pfn[1], pPfnArray->Pfn[2]);

    uShmCmdSize = sizeof(struct shm_cmd) + pPfnArray->uNumberOf4kPages * sizeof(uint32_t);

    pShmCmd = (struct shm_cmd*) malloc(uShmCmdSize);
    if (!pShmCmd)
    {
        errorf("Failed to allocate %d bytes for shm_cmd for window 0x%x\n", uShmCmdSize, hWnd);
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
        pShmCmd->mfns[i] = (uint32_t) pPfnArray->Pfn[i];

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

    debugf("start");
    if (pWatchedDC)
        hWnd = pWatchedDC->hWnd;

    uResult = PrepareShmCmd(pWatchedDC, &pShmCmd);
    if (ERROR_SUCCESS != uResult)
    {
        errorf("PrepareShmCmd() failed with error %d\n", uResult);
        return;
    }

    if (pShmCmd->num_mfn == 0 || pShmCmd->num_mfn > MAX_MFN_COUNT)
    {
        errorf("too large num_mfn=%lu for window 0x%x\n", pShmCmd->num_mfn, (int)hWnd);
        free(pShmCmd);
        return;
    }

    size = pShmCmd->num_mfn * sizeof(uint32_t);

    hdr.type = MSG_MFNDUMP;
    hdr.window = (uint32_t) hWnd;
    hdr.untrusted_len = sizeof(struct shm_cmd) + size;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_struct(hdr);
    write_data(pShmCmd, sizeof(struct shm_cmd) + size);
    LeaveCriticalSection(&g_VchanCriticalSection);

    free(pShmCmd);
}

ULONG send_window_create(PWATCHED_DC pWatchedDC)
{
    WINDOWINFO wi;
    struct msg_hdr hdr;
    struct msg_create mc;
    struct msg_map_info mmi;
    PPFN_ARRAY pPfnArray = NULL;

    wi.cbSize = sizeof(wi);
    /* special case for full screen */
    if (pWatchedDC == NULL)
    {
        QV_GET_SURFACE_DATA_RESPONSE QvGetSurfaceDataResponse;
        ULONG uResult;

        debugf("fullscreen");
        /* TODO: multiple screens? */
        wi.rcWindow.left = 0;
        wi.rcWindow.top = 0;

        pPfnArray = malloc(PFN_ARRAY_SIZE(g_ScreenWidth, g_ScreenHeight));
        pPfnArray->uNumberOf4kPages = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);

        uResult = GetWindowData(NULL, &QvGetSurfaceDataResponse, pPfnArray);
        if (ERROR_SUCCESS != uResult)
        {
            errorf("GetWindowData() failed with error %d\n", uResult);
            return uResult;
        }

        wi.rcWindow.right = QvGetSurfaceDataResponse.uWidth;
        wi.rcWindow.bottom = QvGetSurfaceDataResponse.uHeight;

        free(pPfnArray);

        hdr.window = 0;
    }
    else
    {
        debugf("hwnd=0x%x, (%d,%d)-(%d,%d), override=%d", pWatchedDC->hWnd,
            pWatchedDC->rcWindow.left, pWatchedDC->rcWindow.top,
            pWatchedDC->rcWindow.right, pWatchedDC->rcWindow.bottom,
            pWatchedDC->bOverrideRedirect);

        hdr.window = (uint32_t) pWatchedDC->hWnd;
        wi.rcWindow = pWatchedDC->rcWindow;
    }

    hdr.type = MSG_CREATE;

    mc.x = wi.rcWindow.left;
    mc.y = wi.rcWindow.top;
    mc.width = wi.rcWindow.right - wi.rcWindow.left;
    mc.height = wi.rcWindow.bottom - wi.rcWindow.top;
    mc.parent = (uint32_t) INVALID_HANDLE_VALUE; /* TODO? */
    mc.override_redirect = pWatchedDC ? pWatchedDC->bOverrideRedirect : FALSE;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, mc);

    LeaveCriticalSection(&g_VchanCriticalSection);

    if (pWatchedDC && pWatchedDC->bVisible)
        send_window_map(pWatchedDC);

    if (pWatchedDC)
        send_window_hints(pWatchedDC->hWnd, PPosition); // program-specified position

    return ERROR_SUCCESS;
}

ULONG send_window_destroy(HWND hWnd)
{
    struct msg_hdr hdr;

    debugf("0x%x", hWnd);
    hdr.type = MSG_DESTROY;
    hdr.window = (uint32_t) hWnd;
    hdr.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    write_struct(hdr);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

ULONG send_window_flags(HWND hWnd, uint32_t flags_set, uint32_t flags_unset)
{
    struct msg_hdr hdr;
    struct msg_window_flags flags;

    debugf("0x%x: set 0x%x, unset 0x%x", hWnd, flags_set, flags_unset);
    hdr.type = MSG_WINDOW_FLAGS;
    hdr.window = (uint32_t) hWnd;
    hdr.untrusted_len = 0;
    flags.flags_set = flags_set;
    flags.flags_unset = flags_unset;
    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, flags);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

void send_window_hints(HWND hWnd, uint32_t flags)
{
    struct msg_hdr hdr;
    struct msg_window_hints msg = {0};

    msg.flags = flags;
    debugf("flags: 0x%lx", flags);

    hdr.window = (uint32_t)hWnd;
    hdr.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

void send_screen_hints()
{
    struct msg_hdr hdr;
    struct msg_window_hints msg = {0};

    msg.flags = PMinSize; // minimum size
    msg.min_width = MIN_RESOLUTION_WIDTH;
    msg.min_height = MIN_RESOLUTION_HEIGHT;
    debugf("min %dx%d", msg.min_width, msg.min_height);

    hdr.window = 0; // screen
    hdr.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

ULONG send_window_unmap(HWND hWnd)
{
    struct msg_hdr hdr;

    logf("Unmapping window 0x%x\n", hWnd);

    hdr.type = MSG_UNMAP;
    hdr.window = (uint32_t) hWnd;
    hdr.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    write_struct(hdr);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

ULONG send_window_map(PWATCHED_DC pWatchedDC)
{
    struct msg_hdr hdr;
    struct msg_map_info mmi;

    if (pWatchedDC)
        logf("Mapping window 0x%x\n", pWatchedDC->hWnd);
    else
        logf("Mapping desktop window\n");

    hdr.type = MSG_MAP;
    if (pWatchedDC)
        hdr.window = (uint32_t) pWatchedDC->hWnd;
    else
        hdr.window = 0;
    hdr.untrusted_len = 0;

    if (pWatchedDC && pWatchedDC->ModalParent)
        mmi.transient_for = (uint32_t) pWatchedDC->ModalParent;
    else
        mmi.transient_for = (uint32_t) INVALID_HANDLE_VALUE;

    if (pWatchedDC)
        mmi.override_redirect = pWatchedDC->bOverrideRedirect;
    else
        mmi.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, mmi);
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
            logf("fullscreen window");
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
        debugf("0x%x", pWatchedDC->hWnd);
        hdr.window = (uint32_t) pWatchedDC->hWnd;

        hdr.type = MSG_CONFIGURE;

        mc.x = pWatchedDC->rcWindow.left;
        mc.y = pWatchedDC->rcWindow.top;
        mc.width = pWatchedDC->rcWindow.right - pWatchedDC->rcWindow.left;
        mc.height = pWatchedDC->rcWindow.bottom - pWatchedDC->rcWindow.top;
        mc.override_redirect = 0;
    }
    else // whole screen
    {
        debugf("fullscreen: (0,0) %dx%d", g_ScreenWidth, g_ScreenHeight);
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
        write_message(hdr, mc);
    }

    if (pWatchedDC && pWatchedDC->bVisible)
    {
        mmi.transient_for = (uint32_t) INVALID_HANDLE_VALUE; /* TODO? */
        mmi.override_redirect = pWatchedDC->bOverrideRedirect;

        hdr.type = MSG_MAP;
        write_message(hdr, mmi);
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

// Send screen resolution back do gui daemon.
ULONG send_screen_configure(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    struct msg_hdr hdr;
    struct msg_configure mc;
    struct msg_map_info mmi;

    debugf("(%d,%d) %dx%d", x, y, width, height);
    hdr.window = 0; // 0 = screen

    hdr.type = MSG_CONFIGURE;

    mc.x = x;
    mc.y = y;
    mc.width = width;
    mc.height = height;
    mc.override_redirect = 0;

    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, mc);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return ERROR_SUCCESS;
}

void send_window_damage_event(
    HWND hWnd,
    int x,
    int y,
    int width,
    int height
    )
{
    struct msg_shmimage mx;
    struct msg_hdr hdr;

    debugf("0x%x (%d,%d)-(%d,%d)", hWnd, x, y, x+width, y+height);
    hdr.type = MSG_SHMIMAGE;
    hdr.window = (uint32_t) hWnd;
    mx.x = x;
    mx.y = y;
    mx.width = width;
    mx.height = height;
    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, mx);
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
        StringCchPrintfA(msg.data, RTL_NUMBER_OF(msg.data), "%s (Windows Desktop)", g_HostName);
    }
    debugf("0x%x %S", hWnd, msg.data);

    hdr.window = (uint32_t) hWnd;
    hdr.type = MSG_WMNAME;
    EnterCriticalSection(&g_VchanCriticalSection);
    write_message(hdr, msg);
    LeaveCriticalSection(&g_VchanCriticalSection);
}

void send_protocol_version()
{
    uint32_t version = QUBES_GUI_PROTOCOL_VERSION_WINDOWS;
    write_struct(version);
}

ULONG SetVideoMode(ULONG uWidth, ULONG uHeight, ULONG uBpp)
{
    PWCHAR ptszDeviceName = NULL;
    DISPLAY_DEVICE DisplayDevice;

    if (!IS_RESOLUTION_VALID(uWidth, uHeight))
    {
        errorf("Resolution is invalid: %lux%lu\n", uWidth, uHeight);
        return ERROR_INVALID_PARAMETER;
    }

    logf("New resolution: %lux%lu bpp %u\n", uWidth, uHeight, uBpp);
    // ChangeDisplaySettings fails if thread's desktop != input desktop...
    // This can happen on "quick user switch".
    AttachToInputDesktop();

    if (ERROR_SUCCESS != FindQubesDisplayDevice(&DisplayDevice))
        return perror("FindQubesDisplayDevice");

    ptszDeviceName = (PWCHAR) &DisplayDevice.DeviceName[0];

    logf("DeviceName: %s\n", ptszDeviceName);

    if (ERROR_SUCCESS != SupportVideoMode(ptszDeviceName, uWidth, uHeight, uBpp))
        return perror("SupportVideoMode");

    if (ERROR_SUCCESS != ChangeVideoMode(ptszDeviceName, uWidth, uHeight, uBpp))
        return perror("ChangeVideoMode");

    //debugf("success");
    return ERROR_SUCCESS;
}

ULONG InitVideo(ULONG width, ULONG height, ULONG bpp)
{
    ULONG uResult = SetVideoMode(width, height, bpp);

    if (ERROR_SUCCESS != uResult)
    {
        errorf("SetVideoMode() failed: %lu\n", uResult);

        g_bFullScreenMode = TRUE;

        logf("keeping original resolution %lux%lu\n", g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        g_ScreenWidth = width;
        g_ScreenHeight = height;
    }

    return OpenScreenSection();
}

ULONG ChangeResolution(HDC *screenDC, HANDLE damageEvent, ULONG width, ULONG height, ULONG bpp)
{
    ULONG uResult;

    logf("deinitializing");
    if (ERROR_SUCCESS != (uResult = StopShellEventsThread()))
        return uResult;
    if (ERROR_SUCCESS != (uResult = UnregisterWatchedDC(*screenDC)))
        return uResult;
    if (ERROR_SUCCESS != (uResult = CloseScreenSection()))
        return uResult;
    if (!ReleaseDC(NULL, *screenDC))
        return GetLastError();

    logf("reinitializing");
    if (ERROR_SUCCESS != (uResult = InitVideo(width, height, bpp)))
        return uResult;
    *screenDC = GetDC(NULL);
    if (!(*screenDC))
        return GetLastError();
    if (ERROR_SUCCESS != (uResult = RegisterWatchedDC(*screenDC, damageEvent)))
        return uResult;

    send_pixmap_mfns(NULL); // update framebuffer

    // is it possible to have VM resolution bigger than host set by user?
    if ((g_ScreenWidth < g_HostScreenWidth) && (g_ScreenHeight < g_HostScreenHeight))
        g_bFullScreenMode = TRUE; // can't have reliable/intuitive seamless mode in this case

    HideCursors();
    DisableEffects();

    if (g_bFullScreenMode)
    {
        send_window_map(NULL); // show desktop window
        // FIXME: sometimes after resolution change the desktop in dom0 isn't repainted
        //send_window_damage_event(NULL, 0, 0, g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        if (ERROR_SUCCESS != StartShellEventsThread())
            return uResult;
    }
    logf("done");

    // Reply to the daemon's request (with just the same data).
    // Otherwise daemon won't send screen resize requests again.
    send_screen_configure(g_ResolutionChangeParams.x, g_ResolutionChangeParams.y, width, height);

    return ERROR_SUCCESS;
}

ULONG handle_xconf()
{
    struct msg_xconf xconf;

    //debugf("start");
    read_all_vchan_ext(&xconf, sizeof(xconf));
    logf("host resolution: %lux%lu, mem: %lu, depth: %lu\n", xconf.w, xconf.h, xconf.mem, xconf.depth);
    g_HostScreenWidth = xconf.w;
    g_HostScreenHeight = xconf.h;
    //DebugBreak();
    return InitVideo(xconf.w, xconf.h, 32 /*xconf.depth*/); // FIXME: bpp affects screen section name
}

int bitset(BYTE *keys, int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

BOOL IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

void handle_keymap_notify()
{
    int i;
    WORD win_key;
    unsigned char remote_keys[32];
    INPUT inputEvent;
    int modifier_keys[] = {
        50 /* VK_LSHIFT   */,
        37 /* VK_LCONTROL */,
        64 /* VK_LMENU    */,
        62 /* VK_RSHIFT   */,
        105 /* VK_RCONTROL */,
        108 /* VK_RMENU    */,
        133 /* VK_LWIN     */,
        134 /* VK_RWIN     */,
        135 /* VK_APPS     */,
        0
    };

    debugf("start");
    read_all_vchan_ext((char *) remote_keys, sizeof(remote_keys));
    i = 0;
    while (modifier_keys[i])
    {
        win_key = X11ToVk[modifier_keys[i]];
        if (!bitset(remote_keys, i) && IsKeyDown(X11ToVk[modifier_keys[i]]))
        {
            inputEvent.type = INPUT_KEYBOARD;
            inputEvent.ki.time = 0;
            inputEvent.ki.wScan = 0; /* TODO? */
            inputEvent.ki.wVk = win_key;
            inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
            inputEvent.ki.dwExtraInfo = 0;

            if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
            {
                perror("SendInput");
                return;
            }
            debugf("unsetting key %d\n", win_key);
        }
        i++;
    }
}

// tell helper service to simulate ctrl-alt-del
void SignalSASEvent()
{
    static HANDLE sasEvent = NULL;

    debugf("start");
    if (!sasEvent)
    {
        sasEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, WGA_SAS_EVENT_NAME);
        if (!sasEvent)
            perror("OpenEvent");
    }

    if (sasEvent)
        SetEvent(sasEvent);
}

void handle_keypress(HWND hWnd)
{
    struct msg_keypress key;
    INPUT inputEvent;
    int local_capslock_state;

    debugf("0x%x", hWnd);
    read_all_vchan_ext((char *) &key, sizeof(key));

    /* ignore x, y */
    /* TODO: send to correct window */

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.wScan = 0; /* TODO? */
    inputEvent.ki.dwExtraInfo = 0;

    local_capslock_state = GetKeyState(VK_CAPITAL) & 1;
    // check if remote CapsLock state differs from local
    // other modifiers should be synchronized in MSG_KEYMAP_NOTIFY handler
    if ((!local_capslock_state) ^ (!(key.state & (1<<LockMapIndex))))
    {
        // toggle CapsLock state
        inputEvent.ki.wVk = VK_CAPITAL;
        inputEvent.ki.dwFlags = 0;
        if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
        {
            perror("SendInput(VK_CAPITAL)");
            return;
        }
        inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
        if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
        {
            perror("SendInput(KEYEVENTF_KEYUP)");
            return;
        }
    }

    inputEvent.ki.wVk = X11ToVk[key.keycode];
    inputEvent.ki.dwFlags = key.type == KeyPress ? 0 : KEYEVENTF_KEYUP;

    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        perror("SendInput");
        return;
    }

    // TODO: allow customization of SAS sequence?
    if (IsKeyDown(VK_CONTROL) && IsKeyDown(VK_MENU) && IsKeyDown(VK_HOME))
        SignalSASEvent();
}

void handle_button(HWND hWnd)
{
    struct msg_button button;
    INPUT inputEvent;
    RECT rect = {0};

    debugf("0x%x", hWnd);
    read_all_vchan_ext((char *) &button, sizeof(button));

    if (hWnd)
        GetWindowRect(hWnd, &rect);

    /* TODO: send to correct window */

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.time = 0;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwExtraInfo = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = (button.x + rect.left) * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = (button.y + rect.top) * 65535 / g_ScreenHeight;
    switch (button.button)
    {
    case Button1:
        inputEvent.mi.dwFlags =
            (button.type == ButtonPress) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case Button2:
        inputEvent.mi.dwFlags =
            (button.type == ButtonPress) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case Button3:
        inputEvent.mi.dwFlags =
            (button.type == ButtonPress) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case Button4:
    case Button5:
        inputEvent.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputEvent.mi.mouseData = (button.button == Button4) ? WHEEL_DELTA : -WHEEL_DELTA;
        break;
    default:
        logf("unknown button pressed/released 0x%x\n", button.button);
    }
    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        perror("SendInput");
        return;
    }
}

void handle_motion(HWND hWnd)
{
    struct msg_motion motion;
    INPUT inputEvent;
    RECT rect = {0};

    debugf("0x%x", hWnd);
    read_all_vchan_ext((char *) &motion, sizeof(motion));

    if (hWnd)
        GetWindowRect(hWnd, &rect);

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.time = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = (motion.x + rect.left) * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = (motion.y + rect.top) * 65535 / g_ScreenHeight;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_ABSOLUTE;
    inputEvent.mi.dwExtraInfo = 0;

    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        perror("SendInput");
        return;
    }
}

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
// This thread is created in handle_configure().
DWORD WINAPI ResolutionChangeThread(void *param)
{
    DWORD waitResult;

    while (TRUE)
    {
        // Wait indefinitely for an initial "change resolution" event.
        WaitForSingleObject(g_ResolutionChangeRequestedEvent, INFINITE);
        debugf("resolution change requested: %dx%d", g_ResolutionChangeParams.width, g_ResolutionChangeParams.height);

        do
        {
            // If event is signaled again before timeout expires: ignore and wait for another one.
            waitResult = WaitForSingleObject(g_ResolutionChangeRequestedEvent, RESOLUTION_CHANGE_TIMEOUT);
            debugf("second wait result: %lu", waitResult);
        }
        while (waitResult == WAIT_OBJECT_0);

        // If we're here, that means the wait finally timed out.
        // We can change the resolution now.
        logf("resolution change: %dx%d", g_ResolutionChangeParams.width, g_ResolutionChangeParams.height);
        SetEvent(g_ResolutionChangeEvent); // handled in WatchForEvents
    }
    return 0;
}

void handle_configure(HWND hWnd)
{
    struct msg_configure configure;

    read_all_vchan_ext((char *) &configure, sizeof(configure));
    debugf("0x%x (%d,%d) %dx%d", hWnd, configure.x, configure.y, configure.width, configure.height);
    
    if (hWnd != 0) // 0 is full screen
        SetWindowPos(hWnd, HWND_TOP, configure.x, configure.y, configure.width, configure.height, 0);
    else
    {
        // gui daemon requests screen resize: possible resolution change

        // Create trigger event for the throttling thread if not yet created.
        if (!g_ResolutionChangeRequestedEvent)
            g_ResolutionChangeRequestedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

        // Create the throttling thread if not yet running.
        if (!g_ResolutionChangeThread)
            g_ResolutionChangeThread = CreateThread(NULL, 0, ResolutionChangeThread, NULL, 0, 0);

        if (g_ScreenWidth == configure.width && g_ScreenHeight == configure.height)
        {
            // send ACK to guid so it won't stop sending MSG_CONFIGURE
            send_screen_configure(configure.x, configure.y, configure.width, configure.height);
            return; // nothing changes, ignore
        }

        if (!IS_RESOLUTION_VALID(configure.width, configure.height))
        {
            logf("Ignoring invalid resolution %dx%d", configure.width, configure.height);
            // send back current resolution to keep daemon up to date
            send_screen_configure(0, 0, g_ScreenWidth, g_ScreenHeight);
            return;
        }

        // Store the requested screen size.
        g_ResolutionChangeParams.width = configure.width;
        g_ResolutionChangeParams.height = configure.height;
        // XY coords are used to reply with the same message to the daemon.
        // It's useless for fullscreen but the daemon needs such ACK...
        g_ResolutionChangeParams.x = configure.x;
        g_ResolutionChangeParams.y = configure.y;

        // Signal the trigger event so the throttling thread evaluates the resize request.
        SetEvent(g_ResolutionChangeRequestedEvent);
    }
}

void handle_focus(HWND hWnd)
{
    struct msg_focus focus;

    debugf("0x%x", hWnd);
    read_all_vchan_ext((char *) &focus, sizeof(focus));

    BringWindowToTop(hWnd);
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
}

void handle_close(HWND hWnd)
{
    debugf("0x%x", hWnd);
    PostMessage(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

ULONG handle_server_data()
{
    struct msg_hdr hdr;
    char discard[256];
    int nbRead;

    read_all_vchan_ext((char *)&hdr, sizeof(hdr));

    debugf("received message type %d for 0x%x\n", hdr.type, hdr.window);

    switch (hdr.type)
    {
    case MSG_KEYPRESS:
        handle_keypress((HWND)hdr.window);
        break;
    case MSG_BUTTON:
        handle_button((HWND)hdr.window);
        break;
    case MSG_MOTION:
        handle_motion((HWND)hdr.window);
        break;
    case MSG_CONFIGURE:
        handle_configure((HWND)hdr.window);
        break;
    case MSG_FOCUS:
        handle_focus((HWND)hdr.window);
        break;
    case MSG_CLOSE:
        handle_close((HWND)hdr.window);
        break;
    case MSG_KEYMAP_NOTIFY:
        handle_keymap_notify();
        break;
    default:
        logf("got unknown msg type %d, ignoring\n", hdr.type);

        /* discard unsupported message body */
        while (hdr.untrusted_len > 0)
        {
            nbRead = read_all_vchan_ext(discard, min(hdr.untrusted_len, sizeof(discard)));
            if (nbRead <= 0)
                break;
            hdr.untrusted_len -= nbRead;
        }
    }

    return ERROR_SUCCESS;
}

void SetFullscreenMode()
{
    logf("Full screen mode changed to %d", g_bFullScreenMode);

    // ResetWatch kills the shell event thread and removes all watched windows.
    // If fullscreen is off the shell event thread is also restarted.
    ResetWatch(NULL);

    if (g_bFullScreenMode)
    {
        // show the screen window
        send_window_map(NULL);
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            logf("Changing resolution to match host's");
            g_ResolutionChangeParams.x = g_ResolutionChangeParams.y = 0;
            g_ResolutionChangeParams.width = g_HostScreenWidth;
            g_ResolutionChangeParams.height = g_HostScreenHeight;
            SetEvent(g_ResolutionChangeEvent);
        }
        // hide the screen window
        send_window_unmap(NULL);
    }
}

ULONG WINAPI WatchForEvents()
{
    EVTCHN evtchn;
    OVERLAPPED ol;
    unsigned int fired_port;
    ULONG uEventNumber;
    DWORD i, dwSignaledEvent;
    BOOL bVchanIoInProgress;
    ULONG uResult;
    BOOL bExitLoop;
    HANDLE WatchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE hWindowDamageEvent;
    HANDLE hFullScreenOnEvent;
    HANDLE hFullScreenOffEvent;
    HANDLE hShutdownEvent;
    HDC hDC;
    ULONG uDamage = 0;

    struct shm_cmd *pShmCmd = NULL;

    debugf("start");
    hWindowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    uResult = peer_server_init(6000);
    if (uResult)
    {
        errorf("peer_server_init() failed");
        return ERROR_INVALID_FUNCTION;
    }

    logf("Awaiting for a vchan client, write ring size: %d\n", buffer_space_vchan_ext());

    evtchn = libvchan_fd_for_select(ctrl);

    memset(&ol, 0, sizeof(ol));
    ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    hShutdownEvent = CreateNamedEvent(WGA_SHUTDOWN_EVENT_NAME);
    if (!hShutdownEvent)
        return GetLastError();
    hFullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!hFullScreenOnEvent)
        return GetLastError();
    hFullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!hFullScreenOffEvent)
        return GetLastError();
    g_ResolutionChangeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_ResolutionChangeEvent)
        return GetLastError();

    g_bVchanClientConnected = FALSE;
    bVchanIoInProgress = FALSE;
    bExitLoop = FALSE;

    while (TRUE)
    {
        uEventNumber = 0;

        // Order matters.
        WatchedEvents[uEventNumber++] = hShutdownEvent;
        WatchedEvents[uEventNumber++] = hWindowDamageEvent;
        WatchedEvents[uEventNumber++] = hFullScreenOnEvent;
        WatchedEvents[uEventNumber++] = hFullScreenOffEvent;
        WatchedEvents[uEventNumber++] = g_ResolutionChangeEvent;

        uResult = ERROR_SUCCESS;

        libvchan_prepare_to_select(ctrl);
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!bVchanIoInProgress && !ReadFile(evtchn, &fired_port, 1, NULL, &ol))
        {
            uResult = GetLastError();
            if (ERROR_IO_PENDING != uResult)
            {
                perror("ReadFile");
                bExitLoop = TRUE;
                break;
            }
        }

        bVchanIoInProgress = TRUE;

        WatchedEvents[uEventNumber++] = ol.hEvent;

        dwSignaledEvent = WaitForMultipleObjects(uEventNumber, WatchedEvents, FALSE, INFINITE);
        if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            uResult = perror("WaitForMultipleObjects");
            break;
        }
        else
        {
            if (0 == dwSignaledEvent)
            {
                // shutdown event
                logf("Shutdown event signaled");
                break;
            }

            //debugf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
            switch (dwSignaledEvent)
            {
            case 1: // damage event

                debugf("Damage %d\n", uDamage++);

                if (g_bVchanClientConnected)
                {
                    ProcessUpdatedWindows(TRUE);
                }
                break;

            case 2: // fullscreen on event
                if (g_bFullScreenMode)
                    break; // already in fullscreen
                g_bFullScreenMode = TRUE;
                SetFullscreenMode();
                break;

            case 3: // fullscreen off event
                if (!g_bFullScreenMode)
                    break; // already in seamless
                g_bFullScreenMode = FALSE;
                SetFullscreenMode();
                break;

            case 4: // resolution change event, signaled by ResolutionChangeThread
                ChangeResolution(&hDC, hWindowDamageEvent,
                    g_ResolutionChangeParams.width, g_ResolutionChangeParams.height, 32);
                break;

            case 5: // vchan receive
                // the following will never block; we need to do this to
                // clear libvchan_fd pending state
                //
                // using libvchan_wait here instead of reading fired
                // port at the beginning of the loop (ReadFile call) to be
                // sure that we clear pending state _only_
                // when handling vchan data in this loop iteration (not any
                // other process)
                if (!g_bVchanClientConnected)
                {
                    libvchan_wait(ctrl);

                    bVchanIoInProgress = FALSE;

                    logf("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    uResult = libvchan_server_handle_connected(ctrl);
                    if (uResult)
                    {
                        errorf("libvchan_server_handle_connected() failed");
                        bExitLoop = TRUE;
                        break;
                    }

                    send_protocol_version();

                    // This will probably change the current video mode.
                    if (ERROR_SUCCESS != handle_xconf())
                    {
                        bExitLoop = TRUE;
                        break;
                    }

                    // The screen DC should be opened only after the resolution changes.
                    hDC = GetDC(NULL);
                    uResult = RegisterWatchedDC(hDC, hWindowDamageEvent);
                    if (ERROR_SUCCESS != uResult)
                    {
                        perror("RegisterWatchedDC");
                        bExitLoop = TRUE;
                        break;
                    }

                    // send the whole screen framebuffer map
                    send_window_create(NULL);
                    send_pixmap_mfns(NULL);

                    if (g_bFullScreenMode)
                    {
                        debugf("init in fullscreen mode");
                        send_window_map(NULL);
                    }
                    else
                        if (ERROR_SUCCESS != StartShellEventsThread())
                        {
                            errorf("StartShellEventsThread failed, exiting");
                            bExitLoop = TRUE;
                            break;
                        }

                    g_bVchanClientConnected = TRUE;
                    break;
                }

                if (!GetOverlappedResult(evtchn, &ol, &i, FALSE))
                {
                    if (GetLastError() == ERROR_IO_DEVICE)
                    {
                        // in case of ring overflow, libvchan_wait
                        // will reset the evtchn ring, so ignore this
                        // error as already handled
                        //
                        // Overflow can happen when below loop ("while
                        // (read_ready_vchan_ext())") handle a lot of data
                        // in the same time as qrexec-daemon writes it -
                        // there where be no libvchan_wait call (which
                        // receive the events from the ring), but one will
                        // be signaled after each libvchan_write in
                        // qrexec-daemon. I don't know how to fix it
                        // properly (without introducing any race
                        // condition), so reset the evtchn ring (do not
                        // confuse with vchan ring, which stays untouched)
                        // in case of overflow.
                    }
                    else
                        if (GetLastError() != ERROR_OPERATION_ABORTED)
                        {
                            perror("GetOverlappedResult(evtchn)");
                            bExitLoop = TRUE;
                            break;
                        }
                }

                EnterCriticalSection(&g_VchanCriticalSection);
                libvchan_wait(ctrl);

                bVchanIoInProgress = FALSE;

                if (libvchan_is_eof(ctrl))
                {
                    bExitLoop = TRUE;
                    break;
                }

                while (read_ready_vchan_ext())
                {
                    uResult = handle_server_data();
                    if (ERROR_SUCCESS != uResult)
                    {
                        bExitLoop = TRUE;
                        errorf("handle_server_data() failed: 0x%x", uResult);
                        break;
                    }
                }
                LeaveCriticalSection(&g_VchanCriticalSection);

                break;
            }
        }

        if (bExitLoop)
            break;
    }

    logf("main loop finished");

    if (bVchanIoInProgress)
        if (CancelIo(evtchn))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(ol.hEvent, INFINITE);
        }

    if (!g_bVchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        libvchan_server_handle_connected(ctrl);
    }

    if (g_bVchanClientConnected)
        libvchan_close(ctrl);

    // This is actually CloseHandle(evtchn)

    xc_evtchn_close(ctrl->evfd);

    CloseHandle(ol.hEvent);
    CloseHandle(hWindowDamageEvent);

    StopShellEventsThread();
    UnregisterWatchedDC(hDC);
    CloseScreenSection();
    ReleaseDC(NULL, hDC);
    logf("exiting");

    return bExitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static ULONG CheckForXenInterface()
{
    EVTCHN xc;

    xc = xc_evtchn_open();
    if (INVALID_HANDLE_VALUE == xc)
        return ERROR_NOT_SUPPORTED;
    xc_evtchn_close(xc);
    return ERROR_SUCCESS;
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

    logf("New working set size: %d pages\n", uMaximumWorkingSetSize >> 12);

    return ERROR_SUCCESS;
}

ULONG HideCursors()
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

    debugf("start");
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

ULONG DisableEffects()
{
    ANIMATIONINFO AnimationInfo;

    debugf("start");
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)FALSE, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETDROPSHADOW)");

    AnimationInfo.cbSize = sizeof(AnimationInfo);
    AnimationInfo.iMinAnimate = FALSE;

    if (!SystemParametersInfo(SPI_SETANIMATION, sizeof(AnimationInfo), &AnimationInfo, SPIF_UPDATEINIFILE))
        return perror("SystemParametersInfo(SPI_SETANIMATION)");

    return ERROR_SUCCESS;
}

ULONG ReadRegistryConfig()
{
    HKEY key = NULL;
    DWORD status = ERROR_SUCCESS;
    DWORD type;
    DWORD useDirtyBits;
    DWORD size;
    WCHAR logPath[MAX_PATH];

    // first, read the log directory
    SetLastError(status = RegOpenKey(HKEY_LOCAL_MACHINE, REG_CONFIG_KEY, &key));
    if (status != ERROR_SUCCESS)
    {
        // failed, use some safe default
        // todo: use event log
        log_init(L"c:\\", L"gui-agent");
        logf("registry config: '%s'", REG_CONFIG_KEY);
        return perror("RegOpenKey");
    }

    size = sizeof(logPath) - sizeof(TCHAR);
    RtlZeroMemory(logPath, sizeof(logPath));
    SetLastError(status = RegQueryValueEx(key, REG_CONFIG_LOG_VALUE, NULL, &type, (PBYTE)logPath, &size));
    if (status != ERROR_SUCCESS)
    {
        log_init(L"c:\\", L"gui-agent");
        errorf("Failed to read log path from '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_LOG_VALUE);
        perror("RegQueryValueEx");
        status = ERROR_SUCCESS; // don't fail
        goto cleanup;
    }

    if (type != REG_SZ)
    {
        log_init(L"c:\\", L"gui-agent");
        errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", REG_CONFIG_LOG_VALUE, type);
        status = ERROR_SUCCESS; // don't fail
        goto cleanup;
    }

    log_init(logPath, L"gui-agent");

    // read the rest
    size = sizeof(useDirtyBits);
    logf("reading registry value '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_DIRTY_VALUE);
    SetLastError(status = RegQueryValueEx(key, REG_CONFIG_DIRTY_VALUE, NULL, &type, (PBYTE)&useDirtyBits, &size));
    if (status != ERROR_SUCCESS)
    {
        perror("RegQueryValueEx");
        status = ERROR_SUCCESS; // don't fail, just use default value
        goto cleanup;
    }

    if (type != REG_DWORD)
    {
        errorf("Invalid type of config value '%s', 0x%x instead of REG_DWORD", REG_CONFIG_DIRTY_VALUE, type);
        status = ERROR_SUCCESS; // don't fail, just use default value
        goto cleanup;
    }

    g_bUseDirtyBits = (BOOL) useDirtyBits;

cleanup:
    if (key)
        RegCloseKey(key);

    logf("Use dirty bits? %d", g_bUseDirtyBits);
    return status;
}

ULONG Init()
{
    ULONG uResult;
    WSADATA wsaData;

    if (ERROR_SUCCESS != ReadRegistryConfig())
        return GetLastError();

    debugf("start");
    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    uResult = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != uResult)
    {
        perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    SetLastError(uResult = CheckForXenInterface());
    if (ERROR_SUCCESS != uResult)
    {
        return perror("CheckForXenInterface");
    }

    uResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (uResult == 0)
    {
        if (0 != gethostname(g_HostName, sizeof(g_HostName)))
        {
            errorf("gethostname failed: 0x%x", uResult);
        }
        WSACleanup();
    }
    else
    {
        errorf("WSAStartup failed: 0x%x", uResult);
        // this is not fatal, only used to get host name for full desktop window title
    }

    return ERROR_SUCCESS;
}

int wmain(
    ULONG argc,
    PWCHAR argv[]
)
{
    if (ERROR_SUCCESS != Init())
        return perror("Init");

    InitializeCriticalSection(&g_VchanCriticalSection);

    // Call the thread proc directly.
    if (ERROR_SUCCESS != WatchForEvents())
        return perror("WatchForEvents");

    DeleteCriticalSection(&g_VchanCriticalSection);

    logf("exiting");
    return ERROR_SUCCESS;
}
