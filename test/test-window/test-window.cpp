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

#include <SDKDDKVer.h>
#include <windows.h>
#include <stdlib.h>
#include <strsafe.h>
#include "Resource.h"

// initial window properties
#define WINDOW_X      100
#define WINDOW_Y      100
#define WINDOW_WIDTH  321
#define WINDOW_HEIGHT 231
#define WINDOW_STYLE  WS_OVERLAPPEDWINDOW

#define TEST_STEPS 10
#define TEST_STEP_AMOUNT 20
#define TEST_STEP_DELAY 500

#define MAX_LOADSTRING 100

HINSTANCE g_Instance;
WCHAR g_Caption[MAX_LOADSTRING];
WCHAR g_ClassName[MAX_LOADSTRING];

POINT *g_TestDeltas = NULL;

typedef struct _WINDOW_STATE
{
    HWND WindowHandle;
    DWORD ThreadId;
    RECT Rect;
} WINDOW_STATE;

/*
When position test is active, the window changes its position by TEST_STEP_AMOUNT pixels
every TEST_STEP_DELAY milliseconds following the pattern (repeating):
- increases X by TEST_STEP_AMOUNT, TEST_STEPS times
- increases Y by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases X by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases Y by TEST_STEP_AMOUNT, TEST_STEPS times
- increases X and Y by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases X and Y by TEST_STEP_AMOUNT, TEST_STEPS times

When size test is active, the window changes its size by TEST_STEP_AMOUNT pixels
every TEST_STEP_DELAY milliseconds in the following pattern (repeating):
- increases width by TEST_STEP_AMOUNT, TEST_STEPS times
- increases height by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases width by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases height by TEST_STEP_AMOUNT, TEST_STEPS times
- increases width AND height by TEST_STEP_AMOUNT, TEST_STEPS times
- decreases width AND height by TEST_STEP_AMOUNT, TEST_STEPS times
*/
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);


void PrecalcTests(void)
{
    g_TestDeltas = new POINT[6 * TEST_STEPS];

    for (int i = 0; i < TEST_STEPS; i++)
    {
        g_TestDeltas[i].x = TEST_STEP_AMOUNT;
        g_TestDeltas[i].y = 0;
    }

    for (int i = TEST_STEPS; i < TEST_STEPS * 2; i++)
    {
        g_TestDeltas[i].x = 0;
        g_TestDeltas[i].y = TEST_STEP_AMOUNT;
    }

    for (int i = TEST_STEPS * 2; i < TEST_STEPS * 3; i++)
    {
        g_TestDeltas[i].x = -TEST_STEP_AMOUNT;
        g_TestDeltas[i].y = 0;
    }

    for (int i = TEST_STEPS * 3; i < TEST_STEPS * 4; i++)
    {
        g_TestDeltas[i].x = 0;
        g_TestDeltas[i].y = -TEST_STEP_AMOUNT;
    }

    for (int i = TEST_STEPS * 4; i < TEST_STEPS * 5; i++)
    {
        g_TestDeltas[i].x = TEST_STEP_AMOUNT;
        g_TestDeltas[i].y = TEST_STEP_AMOUNT;
    }

    for (int i = TEST_STEPS * 5; i < TEST_STEPS * 6; i++)
    {
        g_TestDeltas[i].x = -TEST_STEP_AMOUNT;
        g_TestDeltas[i].y = -TEST_STEP_AMOUNT;
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR    lpCmdLine,
    int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    MSG msg;
    HACCEL hAccelTable;

    // Initialize global strings
    LoadString(hInstance, IDS_APP_TITLE, g_Caption, MAX_LOADSTRING);
    LoadString(hInstance, IDC_TESTWINDOW, g_ClassName, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TESTWINDOW));

    // Precalculate move/size test deltas.
    PrecalcTests();

    // Main message loop:
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}

//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TESTWINDOW));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCE(IDC_TESTWINDOW);
    wcex.lpszClassName = g_ClassName;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassEx(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    HWND hWnd;

    g_Instance = hInstance; // Store instance handle in our global variable

    hWnd = CreateWindowW(g_ClassName, g_Caption, WINDOW_STYLE,
        WINDOW_X, WINDOW_Y, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, hInstance, NULL);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

DWORD CALLBACK MoveTestThread(void *param)
{
    WINDOW_STATE *ws = (WINDOW_STATE *) param;
    POINT coords;
    int index = 0;

    // starting window position, updated as we go
    coords.x = ws->Rect.left;
    coords.y = ws->Rect.top;

    while (TRUE)
    {
        MoveWindow(ws->WindowHandle, coords.x, coords.y, ws->Rect.right - ws->Rect.left, ws->Rect.bottom - ws->Rect.top, TRUE);
        coords.x += g_TestDeltas[index].x;
        coords.y += g_TestDeltas[index].y;
        index++;
        if (index == 6 * TEST_STEPS)
            index = 0;
        Sleep(TEST_STEP_DELAY);
    }
}

DWORD CALLBACK SizeTestThread(void *param)
{
    WINDOW_STATE *ws = (WINDOW_STATE *) param;
    POINT size;
    int index = 0;

    // starting window size, updated as we go
    size.x = ws->Rect.right - ws->Rect.left;
    size.y = ws->Rect.bottom - ws->Rect.top;

    while (TRUE)
    {
        MoveWindow(ws->WindowHandle, ws->Rect.left, ws->Rect.top, size.x, size.y, TRUE);
        size.x += g_TestDeltas[index].x;
        size.y += g_TestDeltas[index].y;
        index++;
        if (index == 6 * TEST_STEPS)
            index = 0;
        Sleep(TEST_STEP_DELAY);
    }
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND	- process the application menu
//  WM_PAINT	- Paint the main window
//  WM_DESTROY	- post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static RECT clientRect = { 0 }, windowRect = { 0 };
    static HANDLE moveThread = NULL, sizeThread = NULL;
    static WINDOW_STATE ws = { 0 };

    switch (message)
    {
        case WM_CREATE:
        {
            GetClientRect(hWnd, &clientRect);
            GetWindowRect(hWnd, &windowRect);

            ws.ThreadId = GetCurrentThreadId();
            ws.WindowHandle = hWnd;
            break;
        }

        case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            // Parse the menu selections:
            switch (wmId)
            {
                case IDM_ABOUT:
                    DialogBox(g_Instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                    break;

                case IDM_EXIT:
                    DestroyWindow(hWnd);
                    break;

                default:
                    return DefWindowProc(hWnd, message, wParam, lParam);
            }
            break;
        }

        case WM_WINDOWPOSCHANGED:
        case WM_WINDOWPOSCHANGING:
        case WM_MOVE:
        case WM_MOVING:
        case WM_SIZE:
        case WM_SIZING:
            GetClientRect(hWnd, &clientRect);
            GetWindowRect(hWnd, &windowRect);
            InvalidateRect(hWnd, NULL, FALSE);
            break;

        case WM_PAINT:
        {
            WCHAR text[256];
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            GetClientRect(hWnd, &clientRect);
            GetWindowRect(hWnd, &windowRect);
            StringCbPrintf(text, sizeof(text), L"window: (%d,%d) %dx%d\r\nclient: (%d,%d) %dx%d\r\n"
                L"F2 = toggle menu\r\n"
                L"F3 = toggle position test\r\n"
                L"F4 = toggle size test\r\n"
                L"F5 = flash window\r\n",
                windowRect.left, windowRect.top, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                clientRect.left, clientRect.top, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
            DrawText(hdc, text, -1, &clientRect, DT_LEFT | DT_TOP);
            EndPaint(hWnd, &ps);
            break;
        }

        case WM_KEYUP:
        {
            ws.Rect = windowRect;

            switch (wParam)
            {
                case VK_F2:
                {
                    // toggle menu
                    HMENU menu = GetMenu(hWnd);
                    if (!menu)
                    {
                        menu = LoadMenu(g_Instance, MAKEINTRESOURCE(IDC_TESTWINDOW));
                        SetMenu(hWnd, menu);
                    }
                    else
                    {
                        SetMenu(hWnd, NULL);
                        DestroyMenu(menu);
                    }
                    break;
                }

                case VK_F3:
                {
                    // toggle move test
                    if (moveThread)
                    {
                        TerminateThread(moveThread, 0); // not very gentle but doesn't really matter
                        CloseHandle(moveThread);
                        moveThread = NULL;
                    }
                    else
                    {
                        moveThread = CreateThread(NULL, 0, MoveTestThread, &ws, 0, NULL);
                    }
                    break;
                }

                case VK_F4:
                {
                    // toggle size test
                    if (sizeThread)
                    {
                        TerminateThread(sizeThread, 0); // not very gentle but doesn't really matter
                        CloseHandle(sizeThread);
                        sizeThread = NULL;
                    }
                    else
                    {
                        sizeThread = CreateThread(NULL, 0, SizeTestThread, &ws, 0, NULL);
                    }
                    break;
                }

                case VK_F5:
                {
                    FLASHWINFO fi = { 0 };
                    fi.cbSize = sizeof(fi);
                    fi.hwnd = hWnd;
                    fi.dwFlags = FLASHW_ALL;
                    fi.uCount = 10;
                    fi.dwTimeout = 200;
                    if (!FlashWindowEx(&fi))
                        DebugBreak();
                    break;
                }
            }

            break;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
        case WM_INITDIALOG:
            return (INT_PTR) TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, LOWORD(wParam));
                return (INT_PTR) TRUE;
            }
            break;
    }
    return (INT_PTR) FALSE;
}
