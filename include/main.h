#pragma once
#include <windows.h>

#include "qvcontrol.h"

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

extern BOOL g_bUseDirtyBits;
extern BOOL g_bFullScreenMode;
extern LONG g_ScreenHeight;
extern LONG g_ScreenWidth;
extern LONG g_HostScreenWidth;
extern LONG g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern HWND g_DesktopHwnd;
extern char g_DomainName[256];
extern CRITICAL_SECTION g_csWatchedWindows;

typedef struct _WATCHED_DC
{
    HWND    hWnd;
    HDC     hDC;

    RECT	rcWindow;
    LIST_ENTRY	le;

    BOOL	bVisible;
    BOOL	bOverrideRedirect;
    HWND    ModalParent; // if nonzero, this window is modal in relation to window pointed by this field
    ULONG   uTimeModalChecked; // time of last check for modal window

    BOOL	bStyleChecked;
    ULONG	uTimeAdded;

    BOOL	bIconic;

    LONG	MaxWidth;
    LONG	MaxHeight;
    PPFN_ARRAY	pPfnArray;

} WATCHED_DC, *PWATCHED_DC;

typedef struct _BANNED_POPUP_WINDOWS
{
    ULONG	uNumberOfBannedPopups;
    HWND	hBannedPopupArray[1];
} BANNED_POPUP_WINDOWS, *PBANNED_POPUP_WINDOWS;

// used when searching for modal window that's blocking another window
typedef struct _MODAL_SEARCH_PARAMS
{
    HWND ParentWindow; // window that's disabled by a modal window, input
    HWND ModalWindow; // modal window that's active, output
} MODAL_SEARCH_PARAMS, *PMODAL_SEARCH_PARAMS;

#define InitializeListHead(ListHead) (\
    (ListHead)->Flink = (ListHead)->Blink = (ListHead))

#define RemoveEntryList(Entry) {\
    PLIST_ENTRY _EX_Blink; \
    PLIST_ENTRY _EX_Flink; \
    _EX_Flink = (Entry)->Flink; \
    _EX_Blink = (Entry)->Blink; \
    _EX_Blink->Flink = _EX_Flink; \
    _EX_Flink->Blink = _EX_Blink; \
}

#define RemoveHeadList(ListHead) \
    (ListHead)->Flink; \
{RemoveEntryList((ListHead)->Flink)}

#define IsListEmpty(ListHead) \
    ((ListHead)->Flink == (ListHead))

#define InsertHeadList(ListHead,Entry) {\
    PLIST_ENTRY _EX_Flink; \
    PLIST_ENTRY _EX_ListHead; \
    _EX_ListHead = (ListHead); \
    _EX_Flink = _EX_ListHead->Flink; \
    (Entry)->Flink = _EX_Flink; \
    (Entry)->Blink = _EX_ListHead; \
    _EX_Flink->Blink = (Entry); \
    _EX_ListHead->Flink = (Entry); \
}

#define InsertTailList(ListHead,Entry) {\
    PLIST_ENTRY _EX_Blink; \
    PLIST_ENTRY _EX_ListHead; \
    _EX_ListHead = (ListHead); \
    _EX_Blink = _EX_ListHead->Blink; \
    (Entry)->Flink = _EX_ListHead; \
    (Entry)->Blink = _EX_Blink; \
    _EX_Blink->Flink = (Entry); \
    _EX_ListHead->Blink = (Entry); \
}

ULONG CheckWatchedWindowUpdates(PWATCHED_DC pWatchedDC, WINDOWINFO *pwi, BOOL bDamageDetected, PRECT prcDamageArea);
BOOL ShouldAcceptWindow(HWND hWnd, OPTIONAL WINDOWINFO *pwi);
PWATCHED_DC FindWindowByHwnd(HWND hWnd);
PWATCHED_DC AddWindowWithInfo(HWND hWnd, WINDOWINFO *pwi);
ULONG RemoveWatchedDC(PWATCHED_DC pWatchedDC);
ULONG StartShellEventsThread(void);
ULONG StopShellEventsThread(void);
