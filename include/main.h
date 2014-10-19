#pragma once
#include <windows.h>

#include "qvcontrol.h"

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)
#define QUBES_GUI_PROTOCOL_VERSION_WINDOWS  QUBES_GUI_PROTOCOL_VERSION_LINUX

extern BOOL g_UseDirtyBits;
extern BOOL g_SeamlessMode;
extern LONG g_ScreenHeight;
extern LONG g_ScreenWidth;
extern LONG g_HostScreenWidth;
extern LONG g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern HWND g_DesktopWindow;
extern char g_DomainName[256];
extern CRITICAL_SECTION g_csWatchedWindows;

typedef struct _WATCHED_DC
{
    HWND WindowHandle;
    HDC DC;

    RECT WindowRect;
    LIST_ENTRY ListEntry;

    BOOL IsVisible;
    BOOL IsOverrideRedirect;
    HWND ModalParent; // if nonzero, this window is modal in relation to window pointed by this field
    ULONG TimeModalChecked; // time of last check for modal window

    BOOL IsStyleChecked;
    ULONG TimeAdded;

    BOOL IsIconic;

    LONG MaxWidth;
    LONG MaxHeight;
    PFN_ARRAY *PfnArray;
} WATCHED_DC;

typedef struct _BANNED_WINDOWS
{
    ULONG Count;
    HWND BannedHandles[1]; // variable length
} BANNED_WINDOWS;

// used when searching for modal window that's blocking another window
typedef struct _MODAL_SEARCH_PARAMS
{
    HWND ParentWindow; // window that's disabled by a modal window, input
    HWND ModalWindow; // modal window that's active, output
} MODAL_SEARCH_PARAMS;

#define InitializeListHead(ListHead) (\
    (ListHead)->Flink = (ListHead)->Blink = (ListHead))

#define RemoveEntryList(Entry) {\
    LIST_ENTRY *_EX_Blink; \
    LIST_ENTRY *_EX_Flink; \
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
    LIST_ENTRY *_EX_Flink; \
    LIST_ENTRY *_EX_ListHead; \
    _EX_ListHead = (ListHead); \
    _EX_Flink = _EX_ListHead->Flink; \
    (Entry)->Flink = _EX_Flink; \
    (Entry)->Blink = _EX_ListHead; \
    _EX_Flink->Blink = (Entry); \
    _EX_ListHead->Flink = (Entry); \
}

#define InsertTailList(ListHead,Entry) {\
    LIST_ENTRY *_EX_Blink; \
    LIST_ENTRY *_EX_ListHead; \
    _EX_ListHead = (ListHead); \
    _EX_Blink = _EX_ListHead->Blink; \
    (Entry)->Flink = _EX_ListHead; \
    (Entry)->Blink = _EX_Blink; \
    _EX_Blink->Flink = (Entry); \
    _EX_ListHead->Blink = (Entry); \
}

ULONG CheckWatchedWindowUpdates(
    IN OUT WATCHED_DC *watchedDC,
    IN const WINDOWINFO *windowInfo,
    IN BOOL damageDetected,
    IN const RECT *damageArea
    );

BOOL ShouldAcceptWindow(
    IN HWND window,
    IN const WINDOWINFO *pwi OPTIONAL
    );

WATCHED_DC *FindWindowByHandle(
    HWND hWnd
    );

WATCHED_DC *AddWindowWithInfo(
    IN HWND hWnd,
    IN const WINDOWINFO *windowInfo
    );

ULONG RemoveWatchedDC(WATCHED_DC *pWatchedDC);
ULONG StartShellEventsThread(void);
ULONG StopShellEventsThread(void);
