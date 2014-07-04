#include <windows.h>
#include "libvchan.h"

CRITICAL_SECTION g_VchanCriticalSection;

static struct libvchan *g_Vchan;

int VchanSendBuffer(const void *buf, int size)
{
    int written = 0;
    int ret;

    if (!g_Vchan)
        return -1;

    while (written < size)
    {
        ret = libvchan_write(g_Vchan, (char *)buf + written, size - written);
        if (ret <= 0)
            return ret;

        written += ret;
    }

    return size;
}

int VchanSendMessage(const void *hdr, int size, const void *data, int dataSize)
{
    int ret;
    ret = VchanSendBuffer(hdr, size);
    if (ret <= 0)
        return ret;
    ret = VchanSendBuffer(data, dataSize);
    if (ret <= 0)
        return ret;
    return 0;
}

int VchanReceiveBuffer(void *buf, int size)
{
    int written = 0;
    int ret;
    while (written < size)
    {
        ret = libvchan_read(g_Vchan, (char *)buf + written, size - written);
        if (ret <= 0)
            return ret;

        written += ret;
    }
    return size;
}

int VchanGetReadBufferSize()
{
    return libvchan_data_ready(g_Vchan);
}

int VchanGetWriteBufferSize()
{
    return libvchan_buffer_space(g_Vchan);
}

BOOL VchanInitServer(int port)
{
    g_Vchan = libvchan_server_init(port);
    if (!g_Vchan)
        return FALSE;

    return TRUE;
}

HANDLE VchanGetHandle(void)
{
    return libvchan_fd_for_select(g_Vchan);
}

void VchanPrepareToSelect(void)
{
    libvchan_prepare_to_select(g_Vchan);
}

void VchanWait(void)
{
    libvchan_wait(g_Vchan);
}

BOOL VchanIsServerConnected(void)
{
    return (0 == libvchan_server_handle_connected(g_Vchan));
}

BOOL VchanIsEof(void)
{
    return libvchan_is_eof(g_Vchan);
}

void VchanClose(void)
{
    libvchan_close(g_Vchan);

    // This is actually CloseHandle(vchan)
    xc_evtchn_close(g_Vchan->evfd);
}

ULONG CheckForXenInterface(void)
{
    EVTCHN xc;

    xc = xc_evtchn_open();
    if (INVALID_HANDLE_VALUE == xc)
        return ERROR_NOT_SUPPORTED;
    xc_evtchn_close(xc);
    return ERROR_SUCCESS;
}
