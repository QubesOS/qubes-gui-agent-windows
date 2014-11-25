#include <windows.h>
#include "libvchan.h"
#include "qubes-gui-protocol.h"
#include "log.h"

CRITICAL_SECTION g_VchanCriticalSection;

static struct libvchan *g_Vchan = NULL;

BOOL VchanSendBuffer(IN const void *buffer, IN int size)
{
    int written = 0;
    int status;

    LogVerbose("vchan %p, buf %p, size %d", g_Vchan, buffer, size);

    if (!g_Vchan)
    {
        LogError("vchan not connected");
        return FALSE;
    }

    while (written < size)
    {
        status = libvchan_write(g_Vchan, (char *)buffer + written, size - written);
        if (status < 0)
        {
            LogError("libvchan_write failed: %d", status);
            return FALSE;
        }

        written += status;
    }

    return TRUE;
}

BOOL VchanSendMessage(IN const struct msg_hdr *header, IN int headerSize, IN const void *data, IN int dataSize)
{
    int status;

    LogVerbose("msg 0x%x for window 0x%x, size %d", header->type, header->window, header->untrusted_len);
    status = VchanSendBuffer(header, headerSize);
    if (status < 0)
        return FALSE;

    status = VchanSendBuffer(data, dataSize);
    if (status < 0)
        return FALSE;

    return TRUE;
}

BOOL VchanReceiveBuffer(OUT void *buffer, IN int size)
{
    int written = 0;
    int status;

    LogVerbose("vchan %p, buf %p, size %d", g_Vchan, buffer, size);

    while (written < size)
    {
        status = libvchan_read(g_Vchan, (char *)buffer + written, size - written);
        if (status < 0)
        {
            LogError("libvchan_read failed: %d", status);
            return FALSE;
        }

        written += status;
    }
    return TRUE;
}

int VchanGetReadBufferSize(void)
{
    return libvchan_data_ready(g_Vchan);
}

int VchanGetWriteBufferSize(void)
{
    return libvchan_buffer_space(g_Vchan);
}

BOOL VchanInitServer(IN int port)
{
    g_Vchan = libvchan_server_init(port);
    if (!g_Vchan)
    {
        LogError("vchan not connected");
        return FALSE;
    }

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
