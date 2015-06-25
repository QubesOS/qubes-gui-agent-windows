#include <windows.h>

#include <qubes-gui-protocol.h>
#include <libvchan.h>
#include <vchan-common.h>
#include <log.h>

CRITICAL_SECTION g_VchanCriticalSection;

struct libvchan *g_Vchan = NULL;

BOOL VchanSendMessage(IN const struct msg_hdr *header, IN int headerSize, IN const void *data, IN int dataSize, IN const WCHAR *what)
{
    int status;

    LogVerbose("msg 0x%x (%s) for window 0x%x, size %d", header->type, what, header->window, header->untrusted_len);
    status = VchanSendBuffer(g_Vchan, header, headerSize, what);
    if (status < 0)
        return FALSE;

    status = VchanSendBuffer(g_Vchan, data, dataSize, what);
    if (status < 0)
        return FALSE;

    return TRUE;
}

BOOL VchanInitServer(IN int port)
{
    g_Vchan = libvchan_server_init(0, port, 16384, 16384);
    if (!g_Vchan)
    {
        LogError("libvchan_server_init failed");
        return FALSE;
    }

    return TRUE;
}
