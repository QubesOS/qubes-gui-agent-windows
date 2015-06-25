#pragma once
#include <windows.h>

#include <vchan-common.h>

extern CRITICAL_SECTION g_VchanCriticalSection;
extern struct libvchan *g_Vchan;

BOOL VchanInitServer(IN int port);
BOOL VchanSendMessage(IN const void *header, IN int headerSize, IN const void *data, IN int datasize, IN const WCHAR *what);

#define VCHAN_SEND_MSG(header, body, what) (\
    header.untrusted_len = sizeof(header), \
    VchanSendMessage(&(header), sizeof(header), &(body), sizeof(body), what) \
    )

#define VCHAN_SEND(x, what) VchanSendBuffer(g_Vchan, &(x), sizeof(x), what)
