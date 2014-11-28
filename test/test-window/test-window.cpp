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

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;								// current instance
WCHAR szTitle[MAX_LOADSTRING];					// The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];			// the main window class name

// Forward declarations of functions included in this code module:
ATOM				MyRegisterClass(HINSTANCE hInstance);
BOOL				InitInstance(HINSTANCE, int);
LRESULT CALLBACK	WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK	About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR    lpCmdLine,
    int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.
    MSG msg;
    HACCEL hAccelTable;

    // Initialize global strings
    LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadString(hInstance, IDC_TESTWINDOW, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TESTWINDOW));

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
    wcex.lpszClassName = szWindowClass;
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

    hInst = hInstance; // Store instance handle in our global variable

    hWnd = CreateWindowW(szWindowClass, szTitle, WINDOW_STYLE,
        WINDOW_X, WINDOW_Y, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, hInstance, NULL);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
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
    RECT clientRect = { 0 }, windowRect = { 0 };

    switch (message)
    {
    case WM_CREATE:
    {
        GetClientRect(hWnd, &clientRect);
        GetWindowRect(hWnd, &windowRect);
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
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
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
        StringCbPrintf(text, sizeof(text), L"window: (%d,%d) %dx%d\r\nclient: (%d,%d) %dx%d\r\nF2 = toggle menu",
            windowRect.left, windowRect.top, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
            clientRect.left, clientRect.top, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
        DrawText(hdc, text, -1, &clientRect, DT_LEFT | DT_TOP);
        // TODO: Add any drawing code here...
        EndPaint(hWnd, &ps);
        break;
    }

    case WM_KEYUP:
    {
        if (wParam == VK_F2)
        {
            HMENU menu = GetMenu(hWnd);
            if (!menu)
            {
                menu = LoadMenu(hInst, MAKEINTRESOURCE(IDC_TESTWINDOW));
                SetMenu(hWnd, menu);
            }
            else
            {
                SetMenu(hWnd, NULL);
                DestroyMenu(menu);
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
