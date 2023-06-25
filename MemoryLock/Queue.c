/*++

Module Name:

    queue.c

Abstract:

    This file contains the queue entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, MemoryLockQueueInitialize)
#endif

NTSTATUS
MemoryLockQueueInitialize(
    _In_ WDFDEVICE Device
    )
/*++

Routine Description:

     The I/O dispatch callbacks for the frameworks device object
     are configured in this function.

     A single default I/O Queue is configured for parallel request
     processing, and a driver context memory allocation is created
     to hold our structure QUEUE_CONTEXT.

Arguments:

    Device - Handle to a framework device object.

Return Value:

    VOID

--*/
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;

    PAGED_CODE();

    //
    // Configure a default queue so that requests that are not
    // configure-fowarded using WdfDeviceConfigureRequestDispatching to goto
    // other queues get dispatched here.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
         &queueConfig,
        WdfIoQueueDispatchParallel
        );

    queueConfig.EvtIoDeviceControl = MemoryLockEvtIoDeviceControl;
    queueConfig.EvtIoStop = MemoryLockEvtIoStop;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &queue
                 );

    if(!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "WdfIoQueueCreate failed %!STATUS!", status);
        return status;
    }

    return status;
}

NTSTATUS
LockMemory(
    _In_ DEVICE_CONTEXT* DeviceContext,
    _In_ WDFREQUEST Request
)
{
    MEMORYLOCK_LOCK_IN* input = NULL;
    size_t input_size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(MEMORYLOCK_LOCK_IN), &input, &input_size);
    if (input_size != sizeof(MEMORYLOCK_LOCK_IN))
    {
        DBGPRINT("[ML] Invalid input size %llu\n", input_size);
        return STATUS_INVALID_PARAMETER;
    }

    MEMORYLOCK_LOCK_OUT* output = NULL;
    size_t output_size = 0;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MEMORYLOCK_LOCK_OUT), &output, &output_size);
    if (output_size != sizeof(MEMORYLOCK_LOCK_OUT))
    {
        DBGPRINT("[ML] Invalid output size %llu\n", output_size);
        return STATUS_INVALID_PARAMETER;
    }

    DBGPRINT("[ML] LockMemory: address %p, size 0x%x\n", input->Address, input->Size);

    PMDL mdl = IoAllocateMdl(input->Address, input->Size, FALSE, FALSE, NULL);
    if (!mdl)
    {
        DBGPRINT("[ML] IoAllocateMdl failed\n");
        return STATUS_NO_MEMORY;
    }

    LOCK_ENTRY* entry = (LOCK_ENTRY*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(LOCK_ENTRY), ML_POOL_TAG);
    if (!entry)
    {
        status = STATUS_NO_MEMORY;
        goto fail;
    }
    DBGPRINT("[ML] LockMemory: list entry %p, mdl %p\n", entry, mdl);
    entry->Mdl = mdl;

    try
    {
        MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
        DBGPRINT("[ML] MmProbeAndLockPages OK\n");
        status = STATUS_SUCCESS;
    }
    except(EXCEPTION_EXECUTE_HANDLER)
    {
        DBGPRINT("[ML] MmProbeAndLockPages failed\n");
        status = STATUS_ACCESS_VIOLATION;
    }

    if (!NT_SUCCESS(status))
        goto fail;

    entry->RequestId = ++DeviceContext->LastRequestId;
    entry->Process = PsGetCurrentProcess();

    // only access output after we're done with input, they can both point to the same buffer
    output->RequestId = entry->RequestId;
    output->NumberOfPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(entry->Mdl), MmGetMdlByteCount(entry->Mdl));

    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    FILE_CONTEXT* ctx = (FILE_CONTEXT*)WdfObjectGetTypedContext(file, FILE_CONTEXT);
    DBGPRINT("[ML] Adding request 0x%x, %llu pages, file %p, ctx %p\n",
        output->RequestId, output->NumberOfPages, file, ctx);

    InsertHeadList(&ctx->Requests, &entry->List);
    WdfRequestSetInformation(Request, sizeof(MEMORYLOCK_LOCK_OUT));
    return STATUS_SUCCESS;

fail:
    if (entry)
    {
        RtlZeroMemory(entry, sizeof(*entry));
        ExFreePoolWithTag(entry, ML_POOL_TAG);
    }
    IoFreeMdl(mdl);

    return status;
}

LOCK_ENTRY*
FindRequest(
    _In_ PLIST_ENTRY ListHead,
    _In_ ULONG RequestId
)
{
    for (PLIST_ENTRY node = ListHead->Flink; node != ListHead; node = node->Flink)
    {
        LOCK_ENTRY* entry = CONTAINING_RECORD(node, LOCK_ENTRY, List);
        if (entry->RequestId == RequestId)
            return entry;
    }

    return NULL;
}

NTSTATUS
UnlockMdl(
    PMDL Mdl
)
{
    try
    {
        DBGPRINT("[ML] Unlocking %p, size 0x%x\n", MmGetMdlVirtualAddress(Mdl), MmGetMdlByteCount(Mdl));
        MmUnlockPages(Mdl);
    }
    except(EXCEPTION_EXECUTE_HANDLER)
    {
        DBGPRINT("[ML] MmUnmapLockedPages failed\n");
        // this shouldn't happen and will probably BSOD the system when the process exits with locked pages
        return STATUS_ACCESS_VIOLATION;
    }

    return STATUS_SUCCESS;
}

NTSTATUS UnlockEntry(
    LOCK_ENTRY* Entry
)
{
    if (Entry->Process != PsGetCurrentProcess())
    {
        DBGPRINT("[ML] Request 0x%x: process mismatch\n", Entry->RequestId);
        DbgBreakPoint();
        return STATUS_ACCESS_DENIED;
    }

    NTSTATUS status = UnlockMdl(Entry->Mdl);
    if (!NT_SUCCESS(status))
        return status;

    IoFreeMdl(Entry->Mdl);
    RtlZeroMemory(Entry, sizeof(*Entry));
    ExFreePoolWithTag(Entry, ML_POOL_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS
UnlockMemory(
    _In_ WDFREQUEST Request
    )
{
    DBGPRINT("[ML] UnlockMemory\n");
    MEMORYLOCK_UNLOCK_IN* input = NULL;
    size_t input_size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(MEMORYLOCK_UNLOCK_IN), &input, &input_size);
    if (!NT_SUCCESS(status))
    {
        DBGPRINT("[ML] WdfRequestRetrieveInputBuffer failed: 0x%x\n", status);
        return status;
    }
    if (input_size != sizeof(MEMORYLOCK_UNLOCK_IN))
    {
        DBGPRINT("[ML] Invalid input size %llu\n", input_size);
        return STATUS_INVALID_PARAMETER;
    }

    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    FILE_CONTEXT* ctx = (FILE_CONTEXT*)WdfObjectGetTypedContext(file, FILE_CONTEXT);
    DBGPRINT("[ML] UnlockMemory: file %p, ctx %p\n", file, ctx);
    LOCK_ENTRY* entry = FindRequest(&ctx->Requests, input->RequestId);
    if (!entry)
    {
        DBGPRINT("[ML] Request 0x%x not found\n", input->RequestId);
        return STATUS_INVALID_PARAMETER;
    }

    RemoveEntryList(&entry->List);
    status = UnlockEntry(entry);
    return status;
}

NTSTATUS
GetPfns(
    _In_ WDFREQUEST Request
)
{
    MEMORYLOCK_GET_PFNS_IN* input = NULL;
    size_t input_size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), &input, &input_size);
    if (input_size != sizeof(*input))
    {
        DBGPRINT("[ML] Invalid input size %llu\n", input_size);
        return STATUS_INVALID_PARAMETER;
    }

    DBGPRINT("[ML] GetPfns: request 0x%x\n", input->RequestId);
    // [omeg] Rant: input and output buffers returned by WdfRequestRetrieveXXXBuffer can point
    // to the same thing. It makes sense for direct IO where the driver directly interacts with
    // user memory, but apparently it also happens with buffered IO *and it's not mentioned
    // anywhere in WDF docs*, at least in pages for aforementioned functions OR the general page
    // that describes handling IOCTL buffers. Ugh...
    ULONG id = input->RequestId;

    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    FILE_CONTEXT* ctx = (FILE_CONTEXT*)WdfObjectGetTypedContext(file, FILE_CONTEXT);
    DBGPRINT("[ML] GetPfns: file %p, ctx %p\n", file, ctx);
    LOCK_ENTRY* entry = FindRequest(&ctx->Requests, id);
    if (!entry)
    {
        DBGPRINT("[ML] Request 0x%x not found\n", id);
        return STATUS_INVALID_PARAMETER;
    }

    size_t num_pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(entry->Mdl), MmGetMdlByteCount(entry->Mdl));
    MEMORYLOCK_GET_PFNS_OUT* output = NULL;
    size_t size = sizeof(*output) + num_pages * sizeof(PFN_NUMBER);
    size_t output_size = 0;
    status = WdfRequestRetrieveOutputBuffer(Request, size, &output, &output_size);
    if (output_size != size)
    {
        DBGPRINT("[ML] Invalid output size %llu\n", output_size);
        return STATUS_INVALID_PARAMETER;
    }

    ASSERT(sizeof(ULONG64) == sizeof(PFN_NUMBER));
    output->NumberOfPages = num_pages;
    RtlCopyMemory(&output->Pfn, MmGetMdlPfnArray(entry->Mdl), num_pages * sizeof(PFN_NUMBER));
    DBGPRINT("[ML] GetPfns: request 0x%x, %llu pages (0x%016llx 0x%016llx)\n", id,
        num_pages, output->Pfn[0], output->Pfn[1]);
    WdfRequestSetInformation(Request, size);
    return status;
}

VOID
MemoryLockEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    This event is invoked when the framework receives IRP_MJ_DEVICE_CONTROL request.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    OutputBufferLength - Size of the output buffer in bytes

    InputBufferLength - Size of the input buffer in bytes

    IoControlCode - I/O control code.

Return Value:

    VOID

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* device_context = DeviceGetContext(device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
                "%!FUNC! Queue 0x%p, Request 0x%p, OutputBufferLength %d, InputBufferLength %d, IoControlCode 0x%x",
                Queue, Request, (int) OutputBufferLength, (int) InputBufferLength, IoControlCode);
    DBGPRINT("[ML] IOCTL 0x%x from process %p\n", IoControlCode, PsGetCurrentProcess());

    switch (IoControlCode)
    {
    case IOCTL_MEMORYLOCK_LOCK:
        status = LockMemory(device_context, Request);
        break;

    case IOCTL_MEMORYLOCK_UNLOCK:
        status = UnlockMemory(Request);
        break;

    case IOCTL_MEMORYLOCK_GET_PFNS:
        status = GetPfns(Request);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);

    return;
}

VOID
MemoryLockEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
/*++

Routine Description:

    This event is invoked for a power-managed queue before the device leaves the working state (D0).

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    ActionFlags - A bitwise OR of one or more WDF_REQUEST_STOP_ACTION_FLAGS-typed flags
                  that identify the reason that the callback function is being called
                  and whether the request is cancelable.

Return Value:

    VOID

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Queue 0x%p, Request 0x%p ActionFlags %d",
                Queue, Request, ActionFlags);

    //
    // In most cases, the EvtIoStop callback function completes, cancels, or postpones
    // further processing of the I/O request.
    //
    // Typically, the driver uses the following rules:
    //
    // - If the driver owns the I/O request, it calls WdfRequestUnmarkCancelable
    //   (if the request is cancelable) and either calls WdfRequestStopAcknowledge
    //   with a Requeue value of TRUE, or it calls WdfRequestComplete with a
    //   completion status value of STATUS_SUCCESS or STATUS_CANCELLED.
    //
    //   Before it can call these methods safely, the driver must make sure that
    //   its implementation of EvtIoStop has exclusive access to the request.
    //
    //   In order to do that, the driver must synchronize access to the request
    //   to prevent other threads from manipulating the request concurrently.
    //   The synchronization method you choose will depend on your driver's design.
    //
    //   For example, if the request is held in a shared context, the EvtIoStop callback
    //   might acquire an internal driver lock, take the request from the shared context,
    //   and then release the lock. At this point, the EvtIoStop callback owns the request
    //   and can safely complete or requeue the request.
    //
    // - If the driver has forwarded the I/O request to an I/O target, it either calls
    //   WdfRequestCancelSentRequest to attempt to cancel the request, or it postpones
    //   further processing of the request and calls WdfRequestStopAcknowledge with
    //   a Requeue value of FALSE.
    //
    // A driver might choose to take no action in EvtIoStop for requests that are
    // guaranteed to complete in a small amount of time.
    //
    // In this case, the framework waits until the specified request is complete
    // before moving the device (or system) to a lower power state or removing the device.
    // Potentially, this inaction can prevent a system from entering its hibernation state
    // or another low system power state. In extreme cases, it can cause the system
    // to crash with bugcheck code 9F.
    //

    return;
}
