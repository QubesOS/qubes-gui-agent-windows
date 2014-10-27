#pragma once
#include <windows.h>

extern CRITICAL_SECTION g_VchanCriticalSection;

BOOL CheckForXenInterface(void);
BOOL VchanInitServer(IN int port);
HANDLE VchanGetHandle(void);
BOOL VchanIsServerConnected(void);
void VchanPrepareToSelect(void);
void VchanWait(void);
BOOL VchanIsEof(void);
void VchanClose(void);
int VchanGetReadBufferSize(void);
int VchanReceiveBuffer(OUT void *buffer, IN int size);
int VchanSendBuffer(IN const void *buffer, IN int size);
int VchanGetWriteBufferSize(void);
int VchanSendMessage(IN const void *header, IN int headerSize, IN const void *data, IN int datasize);

#define VCHAN_SEND_MSG(header, body) (\
    header.untrusted_len = sizeof(header), \
    VchanSendMessage(&(header), sizeof(header), &(body), sizeof(body)) \
    )

#define VCHAN_SEND(x) VchanSendBuffer(&(x), sizeof(x))
