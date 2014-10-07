#include <windows.h>
#include "common.h"
#include "qubes-gui-protocol.h"
#include "main.h"
#include "vchan.h"
#include "send.h"
#include "xorg-keymap.h"
#include "resolution.h"
#include "log.h"

// tell helper service to simulate ctrl-alt-del
void SignalSASEvent(void)
{
    static HANDLE sasEvent = NULL;

    LogVerbose("start");
    if (!sasEvent)
    {
        sasEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, WGA_SAS_EVENT_NAME);
        if (!sasEvent)
            perror("OpenEvent");
    }

    if (sasEvent)
    {
        LogDebug("Setting SAS event '%s'", WGA_SAS_EVENT_NAME);
        SetEvent(sasEvent);
    }
}

ULONG handle_xconf(void)
{
    struct msg_xconf xconf;

    LogVerbose("start");
    VchanReceiveBuffer(&xconf, sizeof(xconf));
    LogInfo("host resolution: %lux%lu, mem: %lu, depth: %lu", xconf.w, xconf.h, xconf.mem, xconf.depth);
    g_HostScreenWidth = xconf.w;
    g_HostScreenHeight = xconf.h;
    return SetVideoMode(xconf.w, xconf.h, 32 /*xconf.depth*/); // FIXME: bpp affects screen section name
}

int bitset(BYTE *keys, int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

BOOL IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

void handle_keymap_notify(void)
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

    LogVerbose("start");
    VchanReceiveBuffer((char *) remote_keys, sizeof(remote_keys));
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
            LogDebug("unsetting key %d", win_key);
        }
        i++;
    }
}

void handle_keypress(HWND hWnd)
{
    struct msg_keypress key;
    INPUT inputEvent;
    int local_capslock_state;

    LogVerbose("0x%x", hWnd);
    VchanReceiveBuffer((char *) &key, sizeof(key));

    /* ignore x, y */
    /* TODO: send to correct window */

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.wScan = 0; /* TODO? */
    inputEvent.ki.dwExtraInfo = 0;

    local_capslock_state = GetKeyState(VK_CAPITAL) & 1;
    // check if remote CapsLock state differs from local
    // other modifiers should be synchronized in MSG_KEYMAP_NOTIFY handler
    if ((!local_capslock_state) ^ (!(key.state & (1 << LockMapIndex))))
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
    LogDebug("window 0x%x, VK 0x%x, flags 0x%x", hWnd, inputEvent.ki.wVk, inputEvent.ki.dwFlags);

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
    RECT rect = { 0 };

    LogVerbose("0x%x", hWnd);
    VchanReceiveBuffer((char *) &button, sizeof(button));

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
        LogWarning("unknown button pressed/released 0x%x", button.button);
    }

    LogDebug("window 0x%x, (%d,%d), flags 0x%x", hWnd, inputEvent.mi.dx, inputEvent.mi.dy, inputEvent.mi.dwFlags);
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
    RECT rect = { 0 };

    LogVerbose("0x%x", hWnd);
    VchanReceiveBuffer((char *) &motion, sizeof(motion));

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

void handle_configure(HWND hWnd)
{
    struct msg_configure configure;

    VchanReceiveBuffer((char *) &configure, sizeof(configure));
    LogDebug("0x%x (%d,%d) %dx%d", hWnd, configure.x, configure.y, configure.width, configure.height);

    if (hWnd != 0) // 0 is full screen
        SetWindowPos(hWnd, HWND_TOP, configure.x, configure.y, configure.width, configure.height, 0);
    else
    {
        // gui daemon requests screen resize: possible resolution change

        if (g_ScreenWidth == configure.width && g_ScreenHeight == configure.height)
        {
            // send ACK to guid so it won't stop sending MSG_CONFIGURE
            send_screen_configure(configure.x, configure.y, configure.width, configure.height);
            return; // nothing changes, ignore
        }

        if (!IS_RESOLUTION_VALID(configure.width, configure.height))
        {
            LogWarning("Ignoring invalid resolution %dx%d", configure.width, configure.height);
            // send back current resolution to keep daemon up to date
            send_screen_configure(0, 0, g_ScreenWidth, g_ScreenHeight);
            return;
        }

        // XY coords are used to reply with the same message to the daemon.
        // It's useless for fullscreen but the daemon needs such ACK...
        // Signal the trigger event so the throttling thread evaluates the resize request.
        RequestResolutionChange(configure.width, configure.height, 32, configure.x, configure.y);
    }
}

void handle_focus(HWND hWnd)
{
    struct msg_focus focus;

    LogVerbose("0x%x", hWnd);
    VchanReceiveBuffer((char *) &focus, sizeof(focus));

    BringWindowToTop(hWnd);
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
}

void handle_close(HWND hWnd)
{
    LogDebug("0x%x", hWnd);
    PostMessage(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

void handle_window_flags(HWND hWnd)
{
    struct msg_window_flags flags;

    VchanReceiveBuffer((char *) &flags, sizeof(flags));
    LogDebug("0x%x: set 0x%x, unset 0x%x", hWnd, flags.flags_set, flags.flags_unset);

    if (flags.flags_unset & WINDOW_FLAG_MINIMIZE) // restore
    {
        ShowWindowAsync(hWnd, SW_RESTORE);
    }
    else if (flags.flags_set & WINDOW_FLAG_MINIMIZE) // minimize
    {
        ShowWindowAsync(hWnd, SW_MINIMIZE);
    }
}

ULONG handle_server_data(void)
{
    struct msg_hdr hdr;
    BYTE discard[256];
    int nbRead;

    VchanReceiveBuffer(&hdr, sizeof(hdr));

    LogVerbose("received message type %d for 0x%x", hdr.type, hdr.window);

    switch (hdr.type)
    {
    case MSG_KEYPRESS:
        handle_keypress((HWND) hdr.window);
        break;
    case MSG_BUTTON:
        handle_button((HWND) hdr.window);
        break;
    case MSG_MOTION:
        handle_motion((HWND) hdr.window);
        break;
    case MSG_CONFIGURE:
        handle_configure((HWND) hdr.window);
        break;
    case MSG_FOCUS:
        handle_focus((HWND) hdr.window);
        break;
    case MSG_CLOSE:
        handle_close((HWND) hdr.window);
        break;
    case MSG_KEYMAP_NOTIFY:
        handle_keymap_notify();
        break;
    case MSG_WINDOW_FLAGS:
        handle_window_flags((HWND) hdr.window);
        break;
    default:
        LogWarning("got unknown msg type %d, ignoring", hdr.type);

        /* discard unsupported message body */
        while (hdr.untrusted_len > 0)
        {
            nbRead = VchanReceiveBuffer(discard, min(hdr.untrusted_len, sizeof(discard)));
            if (nbRead <= 0)
                break;
            hdr.untrusted_len -= nbRead;
        }
    }

    return ERROR_SUCCESS;
}
