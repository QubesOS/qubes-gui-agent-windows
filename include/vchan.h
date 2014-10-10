#pragma once
#include <windows.h>

extern CRITICAL_SECTION g_VchanCriticalSection;

BOOL CheckForXenInterface(void);
BOOL VchanInitServer(int port);
HANDLE VchanGetHandle(void);
BOOL VchanIsServerConnected(void);
void VchanPrepareToSelect(void);
void VchanWait(void);
BOOL VchanIsEof(void);
void VchanClose(void);
int VchanGetReadBufferSize(void);
int VchanReceiveBuffer(void *buf, int size);
int VchanSendBuffer(const void *buf, int size);
int VchanGetWriteBufferSize(void);
int VchanSendMessage(const void *hdr, int size, const void *data, int datasize);

#define VCHAN_SEND_MSG(header, body) (\
    header.untrusted_len = sizeof(header), \
    VchanSendMessage(&(header), sizeof(header), &(body), sizeof(body)) \
    )

#define VCHAN_SEND(x) VchanSendBuffer(&(x), sizeof(x))
