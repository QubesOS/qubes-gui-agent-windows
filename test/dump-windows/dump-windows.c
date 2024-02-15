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

#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <dwmapi.h>
#include <stdio.h>

struct ENUM_CTX {
    FILE* fv;
    FILE* fi;
};

#define STYLE_CHECK(ws) if (style & ws) { fprintf(f, #ws); fprintf(f, " "); }

void DumpStyle(FILE* f, DWORD style)
{
    // sorted by flag bit order
    STYLE_CHECK(WS_POPUP);
    STYLE_CHECK(WS_CHILD);
    STYLE_CHECK(WS_MINIMIZE);
    STYLE_CHECK(WS_VISIBLE);
    STYLE_CHECK(WS_DISABLED);
    STYLE_CHECK(WS_CLIPSIBLINGS);
    STYLE_CHECK(WS_CLIPCHILDREN);
    STYLE_CHECK(WS_MAXIMIZE);
    STYLE_CHECK(WS_BORDER);
    STYLE_CHECK(WS_DLGFRAME);
    STYLE_CHECK(WS_VSCROLL);
    STYLE_CHECK(WS_HSCROLL);
    STYLE_CHECK(WS_SYSMENU);
    STYLE_CHECK(WS_THICKFRAME);
    STYLE_CHECK(WS_MINIMIZEBOX); // WS_GROUP
    STYLE_CHECK(WS_MAXIMIZEBOX); // WS_TABSTOP
    STYLE_CHECK(0x8000);
    STYLE_CHECK(0x4000);
    STYLE_CHECK(0x2000);
    STYLE_CHECK(0x1000);
    STYLE_CHECK(0x800);
    STYLE_CHECK(0x400);
    STYLE_CHECK(0x200);
    STYLE_CHECK(0x100);
    STYLE_CHECK(0x80);
    STYLE_CHECK(0x40);
    STYLE_CHECK(0x20);
    STYLE_CHECK(0x10);
    STYLE_CHECK(0x8);
    STYLE_CHECK(0x4);
    STYLE_CHECK(0x2);
    STYLE_CHECK(0x1);
}

// from ReactOS
#define WS_EX_UISTATEFOCUSRECTHIDDEN   0x80000000
#define WS_EX_UISTATEKBACCELHIDDEN     0x40000000
#define WS_EX_REDIRECTED               0x20000000
#define WS_EX_UISTATEACTIVE            0x04000000
#define WS_EX_FORCELEGACYRESIZENCMETR  0x00800000
#define WS_EX_MAKEVISIBLEWHENUNGHOSTED 0x00000800
#define WS_EX_DRAGDETECT               0x00000002

void DumpExStyle(FILE* f, DWORD style)
{
    STYLE_CHECK(WS_EX_UISTATEFOCUSRECTHIDDEN);
    STYLE_CHECK(WS_EX_UISTATEKBACCELHIDDEN);
    STYLE_CHECK(WS_EX_REDIRECTED);
    STYLE_CHECK(0x10000000);
    STYLE_CHECK(WS_EX_NOACTIVATE);
    STYLE_CHECK(WS_EX_UISTATEACTIVE);
    STYLE_CHECK(WS_EX_COMPOSITED);
    STYLE_CHECK(0x01000000);
    STYLE_CHECK(WS_EX_FORCELEGACYRESIZENCMETR);
    STYLE_CHECK(WS_EX_LAYOUTRTL);
    STYLE_CHECK(WS_EX_NOREDIRECTIONBITMAP);
    STYLE_CHECK(WS_EX_NOINHERITLAYOUT);
    STYLE_CHECK(WS_EX_LAYERED);
    STYLE_CHECK(WS_EX_APPWINDOW);
    STYLE_CHECK(WS_EX_STATICEDGE);
    STYLE_CHECK(WS_EX_CONTROLPARENT);
    STYLE_CHECK(0x8000);
    STYLE_CHECK(WS_EX_LEFTSCROLLBAR);
    STYLE_CHECK(WS_EX_RTLREADING);
    STYLE_CHECK(WS_EX_RIGHT);
    STYLE_CHECK(WS_EX_MAKEVISIBLEWHENUNGHOSTED);
    STYLE_CHECK(WS_EX_CONTEXTHELP);
    STYLE_CHECK(WS_EX_CLIENTEDGE);
    STYLE_CHECK(WS_EX_WINDOWEDGE);
    STYLE_CHECK(WS_EX_TOOLWINDOW);
    STYLE_CHECK(WS_EX_MDICHILD);
    STYLE_CHECK(WS_EX_TRANSPARENT);
    STYLE_CHECK(WS_EX_ACCEPTFILES);
    STYLE_CHECK(WS_EX_TOPMOST);
    STYLE_CHECK(WS_EX_NOPARENTNOTIFY);
    STYLE_CHECK(WS_EX_DRAGDETECT);
    STYLE_CHECK(WS_EX_DLGMODALFRAME);
}

int EnumWindowsProc(_In_ HWND window, _In_ LPARAM context)
{
    struct ENUM_CTX* ctx = (struct ENUM_CTX*)context;
    FILE* f;

    if (IsWindowVisible(window))
        f = ctx->fv;
    else
        f = ctx->fi;

    fprintf(f, "%p: ", window);

    if (IsWindowEnabled(window))
        fprintf(f, "E ");
    else
        fprintf(f, "- ");

    if (IsIconic(window))
        fprintf(f, "I ");
    else
        fprintf(f, "- ");

    wchar_t buf[512];
    if (GetClassName(window, buf, ARRAYSIZE(buf)) > 0)
        fprintf(f, "\"%S\" ", buf);
    else
        fprintf(f, "\"\" ");

    if (GetWindowText(window, buf, ARRAYSIZE(buf)) > 0)
        fprintf(f, "\"%S\" ", buf);
    else
        fprintf(f, "\"\" ");

    DWORD pid;
    if (GetWindowThreadProcessId(window, &pid) != 0)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process != INVALID_HANDLE_VALUE)
        {
            DWORD size = ARRAYSIZE(buf);
            if (QueryFullProcessImageName(process, 0, buf, &size))
                fprintf(f, "[%S] ", buf);
            else
                fprintf(f, "[???] ");
            CloseHandle(process);
        }
    }

    RECT rect;
    GetWindowRect(window, &rect);
    fprintf(f, "WR(%d,%d:%d,%d)(%dx%d) ", rect.left, rect.top, rect.right, rect.bottom, rect.right - rect.left, rect.bottom - rect.top);

    WINDOWINFO wi;
    wi.cbSize = sizeof(wi);
    GetWindowInfo(window, &wi);
    fprintf(f, "WI(%d,%d:%d,%d)(%dx%d) ", wi.rcWindow.left, wi.rcWindow.top, wi.rcWindow.right, wi.rcWindow.bottom,
        wi.rcWindow.right - wi.rcWindow.left, wi.rcWindow.bottom - wi.rcWindow.top);
    fprintf(f, "Border(%d,%d) ", wi.cxWindowBorders, wi.cyWindowBorders);

    DWORD cloaked_state;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked_state, sizeof(cloaked_state))))
        if (cloaked_state != 0)
            fprintf(f, "CLOAKED ");

    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        fprintf(f, "DWM(%d,%d:%d,%d)(%dx%d) ", rect.left, rect.top, rect.right, rect.bottom, rect.right - rect.left, rect.bottom - rect.top);
        
    DumpStyle(f, wi.dwStyle);
    DumpExStyle(f, wi.dwExStyle);

    fprintf(f, "\n\n");
    return TRUE;
}

void GetMetrics(FILE* f)
{
    fprintf(f, "System metrics:\n");
    fprintf(f, "  SM_CXBORDER       %d\n", GetSystemMetrics(SM_CXBORDER));
    fprintf(f, "  SM_CYBORDER       %d\n", GetSystemMetrics(SM_CYBORDER));
    fprintf(f, "  SM_CXEDGE         %d\n", GetSystemMetrics(SM_CXEDGE));
    fprintf(f, "  SM_CYEDGE         %d\n", GetSystemMetrics(SM_CYEDGE));
    fprintf(f, "  SM_CXFIXEDFRAME   %d\n", GetSystemMetrics(SM_CXFIXEDFRAME));
    fprintf(f, "  SM_CYFIXEDFRAME   %d\n", GetSystemMetrics(SM_CYFIXEDFRAME));
    fprintf(f, "  SM_CXFOCUSBORDER  %d\n", GetSystemMetrics(SM_CXFOCUSBORDER));
    fprintf(f, "  SM_CYFOCUSBORDER  %d\n", GetSystemMetrics(SM_CYFOCUSBORDER));
    fprintf(f, "  SM_CXPADDEDBORDER %d\n", GetSystemMetrics(SM_CXPADDEDBORDER));
    fprintf(f, "  SM_CXSIZEFRAME    %d\n", GetSystemMetrics(SM_CXSIZEFRAME));
    fprintf(f, "  SM_CYSIZEFRAME    %d\n", GetSystemMetrics(SM_CYSIZEFRAME));
}

int main(void)
{
    struct ENUM_CTX ctx;
    ctx.fv = fopen("windows-visible.txt", "w");
    ctx.fi = fopen("windows-invisible.txt", "w");
    GetMetrics(ctx.fv);
    fprintf(ctx.fv, "Desktop: %p\n", GetDesktopWindow());
    fprintf(ctx.fv, "Shell:   %p\n", GetShellWindow());

    fprintf(ctx.fv, "EnumWindows:\n");
    fprintf(ctx.fi, "EnumWindows:\n");
    EnumWindows(EnumWindowsProc, (LPARAM)&ctx);
}
