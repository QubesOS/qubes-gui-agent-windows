/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "capture.h"
#include "common.h"

#include <log.h>

#include <assert.h>
#include <setupapi.h>
#include <cfgmgr32.h>

BOOL g_CaptureThreadEnable = FALSE;

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout);
static HRESULT ReleaseFrame(IN OUT CAPTURE_CONTEXT* ctx);
static DWORD WINAPI CaptureThread(void* param);
static HANDLE OpenDriverDevice(const GUID* dev_guid);
static MEMORYLOCK_GET_PFNS_OUT* LockMemory(HANDLE device, void* ptr, size_t size);
static void UnlockMemory(HANDLE device, ULONG request_id);

// note: win_perror* functions set last error

static IDXGIAdapter* GetAdapter(void)
{
    LogVerbose("start");
    IDXGIAdapter* ret_adapter = NULL;
    // need the ...1 interfaces for output duplication
    IDXGIFactory1* factory = NULL;
    HRESULT status = CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)(&factory));
    if (FAILED(status))
    {
        win_perror2(status, "CreateDXGIFactory1");
        goto end;
    }

    IDXGIAdapter* adapter = NULL;
    UINT i = 0;
    while (IDXGIFactory1_EnumAdapters(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC desc;
        IDXGIAdapter_GetDesc(adapter, &desc);
        LogDebug("DXGI adapter %d: %s", i, desc.Description);
        // first adapter returned contains primary desktop
        if (i > 0)
            IDXGIAdapter_Release(adapter);
        else
            ret_adapter = adapter;

        i++;
    }

    IDXGIFactory1_Release(factory);
end:
    LogVerbose("end");
    return ret_adapter;
}

static ID3D11Device* GetDevice(IN IDXGIAdapter* adapter)
{
    LogVerbose("start");
    ID3D11Device* device = NULL;

    D3D_FEATURE_LEVEL supported_feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1,
    };

    HRESULT status = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        0, /*D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_DEBUG,*/
        supported_feature_levels, ARRAYSIZE(supported_feature_levels), D3D11_SDK_VERSION,
        &device, NULL, NULL);

    if (FAILED(status))
    {
        win_perror2(status, "D3D11CreateDevice");
        return NULL;
    }

    LogVerbose("end");
    return device;
}

static IDXGIOutput1* GetOutput(IN IDXGIAdapter* adapter)
{
    LogVerbose("start");
    IDXGIOutput* output = NULL;
    UINT i = 0;

    while (IDXGIAdapter1_EnumOutputs(adapter, i, &output) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_OUTPUT_DESC desc = { 0 };

        HRESULT status = IDXGIOutput_GetDesc(output, &desc);
        if (FAILED(status))
        {
            IDXGIOutput_Release(output);
            win_perror2(status, "output->GetDesc()");
            goto fail;
        }

        LogDebug("Output %u: %s, attached to desktop: %d", i, desc.DeviceName, desc.AttachedToDesktop);

        if (desc.AttachedToDesktop)
        {
            // IDXGIOutput1 is needed for output duplication
            IDXGIOutput1* output1 = NULL;
            status = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1);
            if (FAILED(status))
            {
                win_perror2(status, "output->QueryInterface(IDXGIOutput1)");
                IDXGIOutput_Release(output);
                goto fail;
            }

            IDXGIOutput_Release(output);
            LogVerbose("end");
            SetLastError(status);
            return output1;
        }

        IDXGIOutput_Release(output);
        i++;
    }

fail:
    LogVerbose("end");
    SetLastError(DXGI_ERROR_NOT_FOUND);
    return NULL;
}

static IDXGIOutputDuplication* GetDuplication(IN IDXGIOutput1* output, IN ID3D11Device* device, OUT UINT* width, OUT UINT* height)
{
    LogVerbose("start");
    IDXGIOutputDuplication* duplication = NULL;
    HRESULT status = IDXGIOutput1_DuplicateOutput(output, (IUnknown*)device, &duplication);

    if (FAILED(status))
    {
        win_perror2(status, "output->DuplicateOutput()");
        goto fail;
    }

    DXGI_OUTDUPL_DESC desc;
    IDXGIOutputDuplication_GetDesc(duplication, &desc);
    LogDebug("Got output duplication. Surface dimensions = %ux%u %.2f fps, "
        L"format %d, scanline order %d, mapped in memory %d",
        desc.ModeDesc.Width, desc.ModeDesc.Height,
        (float)desc.ModeDesc.RefreshRate.Numerator / desc.ModeDesc.RefreshRate.Denominator,
        desc.ModeDesc.Format, desc.ModeDesc.ScanlineOrdering,
        desc.DesktopImageInSystemMemory);

    if (!desc.DesktopImageInSystemMemory)
    {
        // this should never happen with the basic display driver
        IDXGIOutputDuplication_Release(duplication);
        LogError("TODO: desktop is not in system memory");
        SetLastError(DXGI_ERROR_UNSUPPORTED);
        goto fail;
    }

    *width = desc.ModeDesc.Width;
    *height = desc.ModeDesc.Height;

    LogVerbose("end");
    return duplication;

fail:
    LogVerbose("end");
    return NULL;
}

CAPTURE_CONTEXT* CaptureInitialize(HANDLE frame_event, HANDLE error_event)
{
    LogVerbose("start");
    CAPTURE_CONTEXT* ctx = (CAPTURE_CONTEXT*)calloc(1, sizeof(CAPTURE_CONTEXT));
    if (!ctx)
    {
        LogError("no memory");
        SetLastError(ERROR_OUTOFMEMORY);
        goto fail;
    }

    ctx->mlock = OpenDriverDevice(&GUID_DEVINTERFACE_MemoryLock);
    if (!ctx->mlock)
        goto fail;

    ctx->adapter = GetAdapter();
    if (!ctx->adapter)
        goto fail;

    ctx->device = GetDevice(ctx->adapter);
    if (!ctx->device)
        goto fail;

    ctx->output = GetOutput(ctx->adapter);
    if (!ctx->output)
        goto fail;

    ctx->duplication = GetDuplication(ctx->output, ctx->device, &ctx->width, &ctx->height);
    if (!ctx->duplication)
        goto fail;

    // get one frame to acquire framebuffer map
    if (FAILED(GetFrame(ctx, 5000)))
        goto fail;

    if (FAILED(ReleaseFrame(ctx)))
        goto fail;

    ctx->frame_event = frame_event;
    ctx->ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx->error_event = error_event;

    LogVerbose("end");
    return ctx;

fail:
    CaptureTeardown(ctx);
    LogVerbose("end (%x)", GetLastError());
    return NULL;
}

// preserves last error
void CaptureTeardown(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");

    DWORD status = GetLastError(); // preserve
    InterlockedExchange(&g_CaptureThreadEnable, FALSE);
    if (WaitForSingleObject(ctx->thread, 1000) != WAIT_OBJECT_0)
    { // XXX timeout should be bigger than one in frame acquire
        LogWarning("capture thread timeout");
        TerminateThread(ctx->thread, 0);
    }

    CloseHandle(ctx->ready_event);

    if (ctx->duplication)
        IDXGIOutputDuplication_Release(ctx->duplication);

    if (ctx->output)
        IDXGIOutput1_Release(ctx->output);

    if (ctx->device)
        ID3D11Device_Release(ctx->device);

    if (ctx->adapter)
        IDXGIAdapter_Release(ctx->adapter);

    if (ctx->mlock)
        CloseHandle(ctx->mlock); // locked pages are unlocked here

    if (ctx->frame.texture)
        ReleaseFrame(ctx);

    free(ctx->framebuffer_pfns);
    free(ctx);
    LogVerbose("end");
    SetLastError(status);
}

HRESULT CaptureStart(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");
    HRESULT status = ERROR_SUCCESS;
    InterlockedExchange(&g_CaptureThreadEnable, TRUE);
    ctx->thread = CreateThread(NULL, 0, CaptureThread, ctx, 0, NULL);
    if (!ctx->thread)
    {
        InterlockedExchange(&g_CaptureThreadEnable, FALSE);
        status = win_perror("CreateThread");
    }

    LogVerbose("end");
    return status;
}

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout)
{
    LogVerbose("start");
    assert(!ctx->frame.texture);

    HRESULT status = IDXGIOutputDuplication_AcquireNextFrame(ctx->duplication,
        timeout, &ctx->frame.info, &ctx->frame.texture);
    if (FAILED(status))
    {
        win_perror2(status, "duplication->AcquireNextFrame()");
        goto fail1;
    }

    if (ctx->frame.info.LastPresentTime.QuadPart == 0 && ctx->framebuffer_pfns)
    {
        // only skip here after we mapped the PFNs
        LogVerbose("framebuffer unchanged");
        ctx->frame.mapped = FALSE;
        goto end;
    }

    // we only really need to map the framebuffer to get PFNs
    if (!ctx->framebuffer_pfns)
    {
        LogDebug("1st frame, locking framebuffer");

        status = IDXGIOutputDuplication_MapDesktopSurface(ctx->duplication, &ctx->frame.rect);
        if (FAILED(status))
        {
            win_perror2(status, "duplication->MapDesktopSurface()");
            goto fail2;
        }

        ctx->frame.mapped = TRUE;

        ctx->framebuffer_pfns = LockMemory(ctx->mlock, ctx->frame.rect.pBits, 4 * ctx->width * ctx->height);
        if (!ctx->framebuffer_pfns)
        {
            win_perror("LockMemory");
            goto fail3;
        }
        assert(ctx->framebuffer_pfns->NumberOfPages == FRAMEBUFFER_PAGE_COUNT(ctx->width, ctx->height));
    }

    // dirty rects
    UINT dr_size = 1; // initial buffer can't be empty
    RECT temp_rect;

    // query required size
    ctx->frame.dirty_rects = NULL;
    status = IDXGIOutputDuplication_GetFrameDirtyRects(ctx->duplication, dr_size, &temp_rect, &dr_size);
    if (FAILED(status) && status != DXGI_ERROR_MORE_DATA)
    {
        win_perror2(status, "initial GetFrameDirtyRects");
        goto fail4;
    }

    ctx->frame.dirty_rects = (RECT*)malloc(dr_size);
    if (!ctx->frame.dirty_rects)
    {
        win_perror2(ERROR_OUTOFMEMORY, "allocating dirty rects buffer");
        goto fail4;
    }

    status = IDXGIOutputDuplication_GetFrameDirtyRects(ctx->duplication, dr_size, ctx->frame.dirty_rects, &dr_size);
    if (FAILED(status))
    {
        win_perror2(status, "GetFrameDirtyRects");
        goto fail4;
    }

    ctx->frame.dirty_rects_count = dr_size / sizeof(RECT);
    LogDebug("%u dirty rects", ctx->frame.dirty_rects_count);

    // TODO: GetFrameMoveRects (they seem to always be empty when testing)
    // MSDN note: To produce a visually accurate copy of the desktop,
    // an application must first process all move RECTs before it processes dirty RECTs.

end:
    LogVerbose("end");
    return 0;

fail4:
    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
fail3:
    if (ctx->frame.mapped)
        IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
fail2:
    IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
fail1:
    LogVerbose("end (%x)", status);
    ctx->frame.texture = NULL;
    SetLastError(status);
    return status;
}

static HRESULT ReleaseFrame(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");
    HRESULT status = ERROR_INVALID_PARAMETER;
    if (!ctx->frame.texture)
        goto end;

    if (ctx->frame.mapped)
    {
        status = IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
        if (FAILED(status))
        {
            win_perror2(status, "duplication->UnMapDesktopSurface");
            goto end;
        }
        ctx->frame.mapped = FALSE;
    }

    status = IDXGIResource_Release(ctx->frame.texture);
    if (FAILED(status))
    {
        win_perror2(status, "frame->Release");
        goto end;
    }

    status = IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
    if (FAILED(status))
    {
        win_perror2(status, "duplication->ReleaseFrame");
        goto end;
    }

    ctx->frame.texture = NULL;

    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
    status = ERROR_SUCCESS;
end:
    LogVerbose("end (%x)", status);
    return status;
}

static DWORD WINAPI CaptureThread(void* param)
{
    DWORD status = ERROR_SUCCESS;
    CAPTURE_CONTEXT* capture = (CAPTURE_CONTEXT*)param;
    LogDebug("starting, resolution %ux%u", capture->width, capture->height);

    while (TRUE)
    {
        LogVerbose("loop start");
        if (!InterlockedCompareExchange(&g_CaptureThreadEnable, FALSE, FALSE))
        {
            LogDebug("stopping (disabled)");
            break;
        }

        status = GetFrame(capture, 1000);
        if (FAILED(status))
        {
            if (status == DXGI_ERROR_WAIT_TIMEOUT)
            {
                LogVerbose("frame timeout");
                continue; // no new frame available, wait for next one
            }

            LogWarning("failed to get frame");
            // this usually happens when the capture interface gets invalidated
            // (desktop change etc)
            InterlockedExchange(&g_CaptureThreadEnable, FALSE);
            // notify main loop, it'll reinitialize everything
            SetEvent(capture->error_event);
            break;
        }

        if (capture->frame.dirty_rects_count == 0)
            goto end_frame; // framebuffer contents not changed

        // notify main loop that there's a new frame
        SetEvent(capture->frame_event);

        // wait until main loop processes the frame
        if (WaitForSingleObject(capture->ready_event, 1000) != WAIT_OBJECT_0)
        {
            LogWarning("error/timeout waiting for frame processing");
            // probably something bad happened, exit
            status = ERROR_TIMEOUT;
            ReleaseFrame(capture);
            SetEvent(capture->error_event);
            break;
        }

end_frame:
        status = ReleaseFrame(capture);
        if (FAILED(status))
        {
            LogDebug("signaling error due to failed frame release");
            SetEvent(capture->error_event);
            break;
        }
    }
    LogDebug("exiting");
    return status;
}

// memorylock driver interface
static HANDLE OpenDriverDevice(const GUID* dev_guid)
{
    LogVerbose("start");
    SP_DEVINFO_DATA dev_data = { 0 };
    dev_data.cbSize = sizeof(dev_data);

    HANDLE dev_set = SetupDiGetClassDevs(dev_guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    HANDLE dev_handle = NULL;
    DWORD dev_idx = 0;
    int instance_idx = 0;
    // TODO: more error checking
    while (TRUE)
    {
        BOOL ok = SetupDiEnumDeviceInfo(dev_set, dev_idx, &dev_data);
        if (!ok)
            break;

        dev_data.cbSize = sizeof(dev_data);
        ULONG id_size;
        CM_Get_Device_ID_Size(&id_size, dev_data.DevInst, 0);

        char dev_id[200];
        CM_Get_Device_IDA(dev_data.DevInst, dev_id, sizeof(dev_id), 0);
        LogDebug("Device instance #%d: id = '%S'", dev_data.DevInst, dev_id);
        instance_idx++;

        SP_DEVICE_INTERFACE_DATA iface_data;
        iface_data.cbSize = sizeof(iface_data);
        DWORD iface_idx = 0;
        while (TRUE)
        {
            if (!SetupDiEnumDeviceInterfaces(dev_set, &dev_data, dev_guid, iface_idx, &iface_data))
            {
                win_perror("SetupDiEnumDeviceInterfaces");
                break;
            }

            iface_data.cbSize = sizeof(iface_data);
            DWORD needed;
            SetupDiGetDeviceInterfaceDetail(dev_set, &iface_data, NULL, 0, &needed, 0);

            SP_DEVICE_INTERFACE_DETAIL_DATA* details = (SP_DEVICE_INTERFACE_DETAIL_DATA*)malloc(needed);
            if (!details)
            {
                SetLastError(ERROR_OUTOFMEMORY);
                goto fail;
            }
            details->cbSize = sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);

            DWORD unused;
            ok = SetupDiGetDeviceInterfaceDetail(dev_set, &iface_data, details, needed, &unused, NULL);
            LogDebug("%s", details->DevicePath);

            dev_handle = CreateFile(details->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

            if (dev_handle != INVALID_HANDLE_VALUE)
            {
                LogDebug("Driver device successfully opened");
                LogVerbose("end");
                return dev_handle;
            }

            iface_idx++;
        }

        dev_idx++;
    }

fail:
    LogVerbose("end (fail)");
    return NULL;
}

static MEMORYLOCK_GET_PFNS_OUT* LockMemory(HANDLE device, void* ptr, size_t size)
{
    LogVerbose("start");
    MEMORYLOCK_LOCK_IN input;
    MEMORYLOCK_LOCK_OUT output;
    input.Address = ptr;
    input.Size = (UINT)size;

    if (!DeviceIoControl(device, IOCTL_MEMORYLOCK_LOCK,
        &input, sizeof(input), &output, sizeof(output), NULL, NULL))
    {
        win_perror("DeviceIoControl(IOCTL_MEMORYLOCK_LOCK)");
        goto end;
    }

    LogDebug("request: 0x%x, %llu pages", output.RequestId, output.NumberOfPages);

    MEMORYLOCK_GET_PFNS_IN pfn_in;
    MEMORYLOCK_GET_PFNS_OUT* pfn_out;
    size_t pfn_out_size = sizeof(MEMORYLOCK_GET_PFNS_OUT) + sizeof(PFN_NUMBER) * output.NumberOfPages;
    pfn_out = (MEMORYLOCK_GET_PFNS_OUT*)malloc(pfn_out_size);
    assert(pfn_out);

    pfn_in.RequestId = output.RequestId;

    if (!DeviceIoControl(device, IOCTL_MEMORYLOCK_GET_PFNS,
        &pfn_in, (DWORD)sizeof(pfn_in), pfn_out, (DWORD)pfn_out_size, NULL, NULL))
    {
        win_perror("DeviceIoControl(IOCTL_MEMORYLOCK_GET_PFNS)");
        goto end;
    }

    LogVerbose("PFN array:");
    // XXX DEBUG
    for (size_t i = 0; i < 5; i++)
        LogVerboseRaw("0x%016lx ", pfn_out->Pfn[i]);
    LogVerboseRaw("...\n");

    // we ignore RequestId since we'll not explicitly unlock framebuffer
    // it'll get unlocked by the driver when we close its handle on teardown

end:
    LogVerbose("end");
    return pfn_out;
}

// XXX unused
static void UnlockMemory(HANDLE device, ULONG request_id)
{
    LogDebug("request %x", request_id);
    MEMORYLOCK_UNLOCK_IN input;
    input.RequestId = request_id;

    if (!DeviceIoControl(device, IOCTL_MEMORYLOCK_UNLOCK,
        &input, sizeof(input), NULL, 0, NULL, NULL))
    {
        win_perror("DeviceIoControl(IOCTL_MEMORYLOCK_UNLOCK)");
    }
}
