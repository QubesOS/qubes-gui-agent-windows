/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>

#include "common.h"
#include "main.h"
#include "vchan.h"
#include "vchan-handlers.h"
#include "send.h"
#include "xorg-keymap.h"
#include "resolution.h"

#include <qubes-gui-protocol.h>
#include <log.h>

// tell helper service to simulate ctrl-alt-del
static void SignalSASEvent(void)
{
    static HANDLE sasEvent = NULL;

    LogVerbose("start");
    if (!sasEvent)
    {
        sasEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, QGA_SAS_EVENT_NAME);
        if (!sasEvent)
            win_perror("OpenEvent(" QGA_SAS_EVENT_NAME L")");
    }

    if (sasEvent)
    {
        LogDebug("Setting SAS event '%s'", QGA_SAS_EVENT_NAME);
        SetEvent(sasEvent);
    }
}

DWORD HandleXconf(void)
{
    struct msg_xconf xconf;

    LogVerbose("start");
    if (!VchanReceiveBuffer(g_Vchan, &xconf, sizeof(xconf), L"msg_xconf"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogInfo("host resolution: %lux%lu, mem: %lu, depth: %lu", xconf.w, xconf.h, xconf.mem, xconf.depth);
    g_HostScreenWidth = xconf.w;
    g_HostScreenHeight = xconf.h;
    return SetVideoMode(xconf.w, xconf.h);
}

static int BitSet(IN OUT BYTE *keys, IN int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

static BOOL IsKeyDown(IN int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static DWORD HandleKeymapNotify(void)
{
    int i;
    WORD virtualKey;
    BYTE remoteKeys[32];
    INPUT inputEvent;
    int modifierKeys[] = {
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
    if (!VchanReceiveBuffer(g_Vchan, remoteKeys, sizeof(remoteKeys), L"keymap"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    i = 0;
    while (modifierKeys[i])
    {
        virtualKey = g_X11ToVk[modifierKeys[i]];
        if (!BitSet(remoteKeys, i) && IsKeyDown(g_X11ToVk[modifierKeys[i]]))
        {
            inputEvent.type = INPUT_KEYBOARD;
            inputEvent.ki.time = 0;
            inputEvent.ki.wScan = 0; /* TODO? */
            inputEvent.ki.wVk = virtualKey;
            inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
            inputEvent.ki.dwExtraInfo = 0;

            if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
            {
                return win_perror("SendInput");
            }
            LogDebug("unsetting key VK=0x%x (keycode=0x%x)", virtualKey, modifierKeys[i]);
        }
        i++;
    }
    return ERROR_SUCCESS;
}

// Translates x11 keycode to physical scancode and uses that to synthesize keyboard input.
static DWORD SynthesizeKeycode(IN UINT keycode, IN BOOL release)
{
    WORD scanCode = g_KeycodeToScancode[keycode];
    INPUT inputEvent;

    // If the scancode already has 0x80 bit set, do not send key release.
    if (release && (scanCode & 0x80))
        return ERROR_SUCCESS;

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.dwExtraInfo = 0;
    inputEvent.ki.dwFlags = KEYEVENTF_SCANCODE;
    inputEvent.ki.wVk = 0; // virtual key code is not used
    inputEvent.ki.wScan = scanCode & 0xff;

    LogDebug("%S keycode: 0x%x, scancode: 0x%x, %s", g_KeycodeName[keycode], keycode, scanCode, release ? L"release" : L"press");

    if ((scanCode & 0xff00) == 0xe000) // extended key
    {
        inputEvent.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    if (release)
        inputEvent.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        return win_perror("SendInput");
    }

    return ERROR_SUCCESS;
}

static DWORD HandleKeypress(IN HWND window)
{
    struct msg_keypress keyMsg;
    INPUT inputEvent;
    SHORT localCapslockState;
    DWORD status;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &keyMsg, sizeof(keyMsg), L"msg_keypress"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    /* ignore x, y */
    /* TODO: send to correct window */

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.wScan = 0;
    inputEvent.ki.dwExtraInfo = 0;

    localCapslockState = GetKeyState(VK_CAPITAL) & 1;
    // check if remote CapsLock state differs from local
    // other modifiers should be synchronized in MSG_KEYMAP_NOTIFY handler
    if ((!localCapslockState) ^ (!(keyMsg.state & (1 << LockMapIndex))))
    {
        // toggle CapsLock state
        inputEvent.ki.wVk = VK_CAPITAL;
        inputEvent.ki.dwFlags = 0;
        if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
        {
            return win_perror("SendInput(VK_CAPITAL)");
        }
        inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
        if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
        {
            return win_perror("SendInput(KEYEVENTF_KEYUP)");
        }
    }

    // produce the key press/release
    status = SynthesizeKeycode(keyMsg.keycode, keyMsg.type != KeyPress);
    if (ERROR_SUCCESS != status)
        return status;

    // TODO: allow customization of SAS sequence?
    if (IsKeyDown(VK_CONTROL) && IsKeyDown(VK_SHIFT) && IsKeyDown(VK_DELETE))
        SignalSASEvent();

    return ERROR_SUCCESS;
}

static DWORD HandleButton(IN HWND window)
{
    struct msg_button buttonMsg;
    INPUT inputEvent;
    RECT rect = { 0 };

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &buttonMsg, sizeof(buttonMsg), L"msg_button"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    if (window)
        GetWindowRect(window, &rect);

    /* TODO: send to correct window */

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.time = 0;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwExtraInfo = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = (buttonMsg.x + rect.left) * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = (buttonMsg.y + rect.top) * 65535 / g_ScreenHeight;
    switch (buttonMsg.button)
    {
    case Button1:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case Button2:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case Button3:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case Button4:
    case Button5:
        inputEvent.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputEvent.mi.mouseData = (buttonMsg.button == Button4) ? WHEEL_DELTA : -WHEEL_DELTA;
        break;
    default:
        LogWarning("unknown button pressed/released 0x%x", buttonMsg.button);
    }

    LogDebug("window 0x%x, (%d,%d), flags 0x%x", window, buttonMsg.x, buttonMsg.y, inputEvent.mi.dwFlags);
    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        return win_perror("SendInput");
    }

    return ERROR_SUCCESS;
}

static DWORD HandleMotion(IN HWND window, GetWindowDataCallback getWindowData)
{
    struct msg_motion motionMsg;
    INPUT inputEvent;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &motionMsg, sizeof(motionMsg), L"msg_motion"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    int32_t x = motionMsg.x;
    int32_t y = motionMsg.y;

    if (motionMsg.is_hint)
    {
        LogDebug("0x%x: ignoring motion hint (%d,%d)", window, x, y);
        return ERROR_SUCCESS;
    }

    if (window)
    {
        const WINDOW_DATA* data = getWindowData(window);
        x += data->X;
        y += data->Y;

        LogVerbose("0x%x: (%d,%d)", window, x, y);
    }

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.time = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = x * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = y * 65535 / g_ScreenHeight;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputEvent.mi.dwExtraInfo = 0;

    if (!SendInput(1, &inputEvent, sizeof(inputEvent)))
    {
        return win_perror("SendInput");
    }

    return ERROR_SUCCESS;
}

static DWORD HandleConfigure(IN HWND window)
{
    struct msg_configure configureMsg;

    if (!VchanReceiveBuffer(g_Vchan, &configureMsg, sizeof(configureMsg), L"msg_configure"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogDebug("0x%x: (%d,%d) %dx%d", window, configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height);

    if (window != 0) // 0 is full screen
    {
        UINT flags = SWP_ASYNCWINDOWPOS; // post request without waiting to not block
        EnterCriticalSection(&g_csWatchedWindows);
        WINDOW_DATA* data = FindWindowByHandle(window);
        LeaveCriticalSection(&g_csWatchedWindows);

        if (data != NULL)
        {
            if (data->X == configureMsg.x && data->Y == configureMsg.y)
            {
                flags |= SWP_NOMOVE;
                LogVerbose("SWP_NOMOVE");
            }

            if (data->Width == configureMsg.width && data->Height == configureMsg.height)
            {
                flags |= SWP_NOSIZE;
                LogVerbose("SWP_NOSIZE");
            }
        }
        else
        {
            LogWarning("window 0x%x not tracked", window);
        }

        // XXX HWND_TOP places the window in the z-order top, is this always what we want?
        if (SetWindowPos(window, HWND_TOP, configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height, flags))
        {
            EnterCriticalSection(&g_csWatchedWindows);
            WINDOW_DATA* data = FindWindowByHandle(window);
            if (!data)
            {
                LogWarning("window 0x%x not tracked", window);
                LeaveCriticalSection(&g_csWatchedWindows);
                goto end;
            }

            // update expected pos/size of the tracked window without waiting for actual change
            // since we use SWP_ASYNCWINDOWPOS
            // TODO: improve further, somehow window data via UpdateWindowData() later after SetWindowPos below
            // is still different from what was passed via configureMsg - DWM API needs a while to properly update
            // the window state after changes?
            if (!(flags & SWP_NOMOVE))
            {
                LogVerbose("Updating position of 0x%x: (%d,%d) -> (%d,%d)", window, data->X, data->Y,
                    configureMsg.x, configureMsg.y);
                data->X = configureMsg.x;
                data->Y = configureMsg.y;
            }

            if (!(flags & SWP_NOSIZE))
            {
                LogVerbose("Updating size of 0x%x: %dx%d -> %dx%d", window, data->Width, data->Height,
                    configureMsg.width, configureMsg.height);
                data->Width = configureMsg.width;
                data->Height = configureMsg.height;
            }
            LeaveCriticalSection(&g_csWatchedWindows);
        }
        else
        {
            win_perror("SetWindowPos");
        }
    }
    else
    {
        // gui daemon requests screen resize: possible resolution change

        if (g_ScreenWidth == configureMsg.width && g_ScreenHeight == configureMsg.height)
        {
            goto end;
            // nothing changes, ignore
        }

        if (!IS_RESOLUTION_VALID(configureMsg.width, configureMsg.height))
        {
            LogWarning("Ignoring invalid resolution %dx%d", configureMsg.width, configureMsg.height);
            goto end;
        }

        // XY coords are used to reply with the same message to the daemon.
        // It's useless for fullscreen but the daemon needs such ACK...
        // Signal the trigger event so the throttling thread evaluates the resize request.
        RequestResolutionChange(configureMsg.width, configureMsg.height, configureMsg.x, configureMsg.y);
    }
end:
    // send ACK to gui daemon so it won't stop sending MSG_CONFIGURE
    return SendWindowConfigure(window,
        configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height, configureMsg.override_redirect);
}

static DWORD HandleFocus(IN HWND window)
{
    struct msg_focus focusMsg;

    if (!VchanReceiveBuffer(g_Vchan, &focusMsg, sizeof(focusMsg), L"msg_focus"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogVerbose("0x%x: type %x, mode %x, detail %x", window, focusMsg.type, focusMsg.mode, focusMsg.detail);

    if (focusMsg.type == 9) // focus gain
    {
        BringWindowToTop(window);
        SetForegroundWindow(window);
    }

    return ERROR_SUCCESS;
}

static DWORD HandleClose(IN HWND window)
{
    LogDebug("0x%x", window);
    PostMessage(window, WM_SYSCOMMAND, SC_CLOSE, 0);

    return ERROR_SUCCESS;
}

static DWORD HandleWindowFlags(IN HWND window)
{
    struct msg_window_flags flagsMsg;

    if (!VchanReceiveBuffer(g_Vchan, &flagsMsg, sizeof(flagsMsg), L"msg_window_flags"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogDebug("0x%x: set 0x%x, unset 0x%x", window, flagsMsg.flags_set, flagsMsg.flags_unset);

    if (flagsMsg.flags_unset & WINDOW_FLAG_MINIMIZE) // restore
    {
        ShowWindowAsync(window, SW_RESTORE);
    }
    else if (flagsMsg.flags_set & WINDOW_FLAG_MINIMIZE) // minimize
    {
        ShowWindowAsync(window, SW_MINIMIZE);
    }

    return ERROR_SUCCESS;
}

DWORD HandleServerData(GetWindowDataCallback getWindowData)
{
    struct msg_hdr header;
    BYTE discardBuffer[256];
    int readSize;
    DWORD status = ERROR_SUCCESS;

    if (!VchanReceiveBuffer(g_Vchan, &header, sizeof(header), L"msg_hdr"))
    {
        LogError("VchanReceiveBuffer failed");
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogVerbose("received message type %d for 0x%x", header.type, header.window);

#pragma warning(push)
#pragma warning(disable:4312)
    switch (header.type)
    {
    case MSG_KEYPRESS:
        status = HandleKeypress((HWND)header.window);
        break;
    case MSG_BUTTON:
        status = HandleButton((HWND)header.window);
        break;
    case MSG_MOTION:
        status = HandleMotion((HWND)header.window, getWindowData);
        break;
    case MSG_CONFIGURE:
        status = HandleConfigure((HWND)header.window);
        break;
    case MSG_FOCUS:
        status = HandleFocus((HWND)header.window);
        break;
    case MSG_CLOSE:
        status = HandleClose((HWND)header.window);
        break;
    case MSG_KEYMAP_NOTIFY:
        status = HandleKeymapNotify();
        break;
    case MSG_WINDOW_FLAGS:
        status = HandleWindowFlags((HWND)header.window);
        break;
#pragma warning(pop)
    default:
        LogWarning("got unknown msg type %d, ignoring", header.type);

        /* discard unsupported message body */
        while (header.untrusted_len > 0)
        {
            readSize = min(header.untrusted_len, sizeof(discardBuffer));
            if (!VchanReceiveBuffer(g_Vchan, discardBuffer, readSize, L"discard buffer"))
            {
                LogError("VchanReceiveBuffer failed");
                return ERROR_UNIDENTIFIED_ERROR;
            }
            header.untrusted_len -= readSize;
        }
    }

    if (ERROR_SUCCESS != status)
    {
        LogError("handler failed: 0x%x, exiting", status);
    }

    return status;
}
