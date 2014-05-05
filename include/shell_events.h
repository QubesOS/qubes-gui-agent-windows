#pragma once

#include <windows.h>
#include <tchar.h>
#include <WtsApi32.h>
#include "qvcontrol.h"
#include "common.h"

#define REG_CONFIG_KEY L"Software\\Invisible Things Lab\\Qubes Tools"

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

extern LONG g_ScreenWidth;
extern LONG g_ScreenHeight;
extern BOOL g_bFullScreenMode;
extern BOOL g_bUseDirtyBits;
extern BOOL g_bVchanClientConnected;
extern PQV_DIRTY_PAGES g_pDirtyPages;

ULONG AttachToInputDesktop();

ULONG StartShellEventsThread();

ULONG StopShellEventsThread();

ULONG ProcessUpdatedWindows(
    BOOL bUpdateEverything
);

ULONG send_window_create(
    PWATCHED_DC	pWatchedDC
);

void send_pixmap_mfns(
    PWATCHED_DC pWatchedDC
);

void send_window_damage_event(
    HWND window,
    int x,
    int y,
    int width,
    int height
);

void send_wmname(
    HWND window
);

ULONG send_window_configure(
    PWATCHED_DC pWatchedDC
);

ULONG send_window_destroy(
    HWND window
);

ULONG send_window_unmap(
    HWND window
);

ULONG send_window_map(
    PWATCHED_DC pWatchedDC
);

DWORD WINAPI ResetWatch(PVOID param);

#define InitializeListHead(ListHead) (\
    (ListHead)->Flink = (ListHead)->Blink = (ListHead))

#define RemoveEntryList(Entry) {\
    PLIST_ENTRY _EX_Blink;\
    PLIST_ENTRY _EX_Flink;\
    _EX_Flink = (Entry)->Flink;\
    _EX_Blink = (Entry)->Blink;\
    _EX_Blink->Flink = _EX_Flink;\
    _EX_Flink->Blink = _EX_Blink;\
    }

#define RemoveHeadList(ListHead) \
    (ListHead)->Flink;\
    {RemoveEntryList((ListHead)->Flink)}

#define IsListEmpty(ListHead) \
    ((ListHead)->Flink == (ListHead))

#define InsertHeadList(ListHead,Entry) {\
    PLIST_ENTRY _EX_Flink;\
    PLIST_ENTRY _EX_ListHead;\
    _EX_ListHead = (ListHead);\
    _EX_Flink = _EX_ListHead->Flink;\
    (Entry)->Flink = _EX_Flink;\
    (Entry)->Blink = _EX_ListHead;\
    _EX_Flink->Blink = (Entry);\
    _EX_ListHead->Flink = (Entry);\
    }

#define InsertTailList(ListHead,Entry) {\
    PLIST_ENTRY _EX_Blink;\
    PLIST_ENTRY _EX_ListHead;\
    _EX_ListHead = (ListHead);\
    _EX_Blink = _EX_ListHead->Blink;\
    (Entry)->Flink = _EX_ListHead;\
    (Entry)->Blink = _EX_Blink;\
    _EX_Blink->Flink = (Entry);\
    _EX_ListHead->Blink = (Entry);\
    }
