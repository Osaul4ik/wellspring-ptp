// Queue entry points and callbacks. Kernel-mode Driver Framework
//
// ---------------------------------------------------------------------
// InputQueue LIFECYCLE POLICY (non-power-managed by design - see below)
// ---------------------------------------------------------------------
// InputQueue (DEVICE_CONTEXT::InputQueue) is a manual WdfIoQueueDispatchManual
// queue with PowerManaged = FALSE. This is intentional, not an oversight:
// a HID minidriver's read queue must be able to hold a pending
// IOCTL_HID_READ_REPORT request ACROSS a D0Exit/D0Entry (sleep/resume)
// cycle so the request is still there, ready to be completed with fresh
// data, the moment the device comes back - a power-managed queue would
// instead stop delivering to/from this queue around every power
// transition, which is not what a continuously-open HID read pipe wants.
//
// Because it is non-power-managed, WDF does NOT call AmtPtpDeviceUsbKmEvtIoStop
// on it for ordinary D0Exit/D0Entry power transitions - only for the
// framework-driven stop/purge that happens around device
// PnP stop (surprise removal) and remove. Concretely, per code path:
//
//   - D0Exit (sleep, or any power-down): InputQueue is left untouched.
//     Pending HID read requests simply stay queued. No owner change, no
//     cancellation, no completion. This is deliberate: the queue's
//     contents are not tied to a USB session, only to the FDO's own
//     lifetime.
//   - D0Entry (resume): same - InputQueue is not touched here either.
//     Any request that was pending across the sleep is still pending and
//     will be satisfied by the next successful interrupt-pipe read
//     completion once the reader restarts.
//   - Surprise removal / PnP stop-for-remove: WDF calls
//     AmtPtpDeviceUsbKmEvtIoStop with WdfRequestStopActionPurge for every
//     request still on InputQueue. Each is completed here with
//     STATUS_CANCELLED - this driver owns cancellation of its own queue's
//     contents; WDF does not do this automatically. No stale request can
//     survive past this point to be completed with data belonging to the
//     old (now-torn-down) hardware session.
//   - PnP suspend (queue-level, distinct from device D0Exit): WDF calls
//     EvtIoStop with WdfRequestStopActionSuspend. The request is
//     acknowledged (WdfRequestStopAcknowledge(Request, FALSE)) and left on
//     the queue for the framework to redeliver later - no ownership
//     change.
//   - Normal completion: whichever HID-read-request consumer actually
//     satisfies the request - AmtPtpEvtUsbInterruptPipeReadComplete
//     (Interrupt.c), on both the digitizer and the opportunistic
//     force-touch mouse delivery path - retrieves it via
//     WdfIoQueueRetrieveNextRequest and completes it directly. That
//     completion is the ONLY path that ever hands report data back for a
//     request pulled from this queue; Queue.c itself never completes a
//     forwarded read with data, only with STATUS_CANCELLED (purge) or not
//     at all (suspend/normal forward).
//
// Ownership summary per request lifetime: forwarded to InputQueue by
// AmtPtpDispatchReadReportRequests (below); requeue/redelivery owned by
// WDF itself (suspend path); cancel-on-remove owned by
// AmtPtpDeviceUsbKmEvtIoStop (this file); data-completion owned by
// AmtPtpEvtUsbInterruptPipeReadComplete (Interrupt.c). No other function
// in this driver retrieves from or completes requests on InputQueue.

#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmQueueInitialize)
#endif

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    )
// Creates default parallel queue and manual queue for touch reads.
//
// Both WdfIoQueueCreate calls below are routed through
// AmtPtpRetryOnLowResourcesCtx (Device.h/Device.c) - the same bounded,
// STATUS_INSUFFICIENT_RESOURCES-only retry every other allocation point
// on the AddDevice-time install path already gets. This function is
// called from AmtPtpDeviceUsbKmCreateDevice, still inside EvtDriverDeviceAdd's
// call chain, so the same "no PnP Start to retry, AddDevice just fails
// outright" gap applies here as much as it does to CreateDevice's own
// timer/pool/lock allocations or AmtPtpAcquireConfigControlDevice's
// control-device create.
{
    WDFQUEUE                queue;
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    PDEVICE_CONTEXT         pDeviceContext;
    AMT_QUEUE_CREATE_CTX    queueCtx;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);
    
    // Default queue for non-forwarded requests.
    //
    // EvtIoDeviceControl (external/user-mode-originated IOCTLs) is
    // deliberately NOT wired up here: this FDO is a lower filter on the
    // HIDClass stack with no device interface of its own (see Public.h),
    // so no user-mode caller can ever reach it directly. The
    // AmtPtpConfigGui-facing IOCTL_AMT_PTP_* surface is served exclusively
    // by the separate KMDF control device - see
    // AmtPtpConfigControlEvtIoDeviceControl in ConfigIoctl.c.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = AmtPtpDeviceUsbKmEvtIoDeviceControl;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

    queueCtx.Device      = Device;
    queueCtx.QueueConfig = &queueConfig;
    queueCtx.OutQueue    = &queue;

    status = AmtPtpRetryOnLowResourcesCtx(
        pDeviceContext,
        "QueueInitialize: WdfIoQueueCreate(default)",
        AmtPtpQueueCreateAttempt,
        &queueCtx);

    if( !NT_SUCCESS(status) ) {
        return status;
    }

    // Manual queue for forwarded HID read requests.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

    queueCtx.Device      = Device;
    queueCtx.QueueConfig = &queueConfig;
    queueCtx.OutQueue    = &pDeviceContext->InputQueue;

    status = AmtPtpRetryOnLowResourcesCtx(
        pDeviceContext,
        "QueueInitialize: WdfIoQueueCreate(InputQueue)",
        AmtPtpQueueCreateAttempt,
        &queueCtx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // A queued read may arrive after the final touch interrupt. Notify the
    // force-touch delivery state machine so a pending mouse DOWN/UP report
    // can be delivered without requiring another physical touch.
    status = WdfIoQueueReadyNotify(
        pDeviceContext->InputQueue,
        AmtPtpEvtInputQueueReady,
        pDeviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // NOTE: a separate manual "MouseInputQueue" for the force-touch mouse
    // top-level collection existed here between the "Fullfix" commit and
    // this revert. It has been removed - the mouse collection's reads are
    // forwarded to the same InputQueue as the digitizer, matching the
    // pre-Fullfix behavior (see AmtPtpDispatchReadReportRequests and
    // AmtPtpEvtUsbInterruptPipeReadComplete).

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
// Dispatches HID IOCTLs to handler functions.
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(device);
    BOOLEAN requestPending = FALSE;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // DIAG: every IOCTL this filter sees except IOCTL_HID_READ_REPORT,
    // which is the per-frame hot path and would flood DebugView64 if
    // traced here. Everything else - descriptors, attributes, GET/
    // SET_FEATURE - only happens around enumeration and wake, so tracing
    // it (still gated by the usual DebugMode switch in AmtTrace()) is
    // cheap and gives the wake-time IOCTL timeline needed to correlate
    // against a System-log "MTConfig" failure - see the
    // WELLSPRING_MODE_SETFEATURE_* comment in Device.h.
    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        BOOLEAN lifecycleBlocked;

        BOOLEAN t2RestartPending;

        WdfSpinLockAcquire(pDeviceContext->D0ExitLock);
        lifecycleBlocked =
            pDeviceContext->D0ExitInProgress ||
            pDeviceContext->T2RestartPending ||
            pDeviceContext->T2ResumeActive;
        t2RestartPending = pDeviceContext->T2RestartPending;
        WdfSpinLockRelease(pDeviceContext->D0ExitLock);

        AmtTrace(pDeviceContext,
            "EvtIoDeviceControl: ENTER, IoControlCode=0x%08X",
            IoControlCode);

        // Do not start new HID control/feature work against the old hardware
        // lifecycle once T2 re-enumeration has been requested, or while the
        // D3->D0 T2 startup gate is active. READ_REPORT is deliberately not
        // rejected here: its manual input queue is non-power-managed and
        // pending reads belong to the FDO lifetime; PnP purge owns their
        // cancellation when the old stack is removed.
        if (lifecycleBlocked) {
            // Once T2 recovery has explicitly latched T2RestartPending, the
            // current FDO is already on the path to PnP re-enumeration.
            // MTConfig's Windows Device Mode SET_FEATURE is the one control
            // request we can safely complete without touching the old
            // hardware: the replacement device will run the normal HID
            // configuration sequence after re-enumeration.
            //
            // Do NOT do this for ordinary D0Exit/T2ResumeActive states: a
            // restart has not necessarily been committed there, so lying
            // about successful Device Mode configuration could leave the
            // current device in an incorrect mode.
            if (t2RestartPending &&
                IoControlCode == IOCTL_HID_SET_FEATURE) {
                // Let AmtPtpSetFeatures() inspect and validate the HID
                // report itself. It has the existing report-size validation
                // and will fake-success ONLY for the Windows Device Mode
                // report; other SET_FEATURE reports still get rejected or
                // handled normally there.
                AmtTrace(pDeviceContext,
                    "EvtIoDeviceControl: T2 restart pending, allowing "
                    "SET_FEATURE to reach the HID handler for Device Mode filtering");
            } else {
                AmtTrace(pDeviceContext,
                    "EvtIoDeviceControl: lifecycle transition/restart pending, "
                    "rejecting IoControlCode=0x%08X", IoControlCode);
                WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
                return;
            }
        }
    }

    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = AmtPtpGetHidDescriptor(device, Request);
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = AmtPtpGetDeviceAttribs(device, Request);
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = AmtPtpGetReportDescriptor(device, Request);
        break;
    case IOCTL_HID_READ_REPORT:
        status = AmtPtpDispatchReadReportRequests(device, Request, &requestPending);
        break;
    case IOCTL_HID_GET_FEATURE:
        status = AmtPtpReportFeatures(device, Request);
        break;
    case IOCTL_HID_SET_FEATURE:
        status = AmtPtpSetFeatures(device, Request);
        break;
    case IOCTL_HID_GET_STRING:
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
    case IOCTL_UMDF_HID_GET_INPUT_REPORT:
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
    default:
        AmtTrace(pDeviceContext, "EvtIoDeviceControl: unsupported IoControlCode=0x%08X", IoControlCode);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        AmtTrace(pDeviceContext, "EvtIoDeviceControl: EXIT, IoControlCode=0x%08X, status=0x%08X",
            IoControlCode, status);
    }

    if (requestPending != TRUE) {
        WdfRequestComplete(Request, status);
    }

    return;
}

NTSTATUS
AmtPtpDispatchReadReportRequests(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ BOOLEAN* Pending
)
// Forwards HID read requests to the manual input queue.
{
    NTSTATUS status;
    PDEVICE_CONTEXT pDevContext;

    pDevContext = DeviceGetContext(Device);

    // REVERT (force-touch regression fix): the buffer-size-based split that
    // routed the mouse top-level collection's reads to a separate
    // MouseInputQueue was introduced in the "Fullfix" rework and is the
    // most likely cause of the intermittent (every-other-press, requiring
    // a full finger lift to recover) force-touch delivery failures - the
    // mouse collection's read cadence from mouhid.sys is not guaranteed to
    // keep a request continuously queued at the exact moment
    // AmtPtpEvtUsbInterruptPipeReadComplete needs one, so the down/up pulse
    // could desync from the digitizer-driven delivery loop.
    //
    // Restored to the pre-Fullfix design: every HID read request (both the
    // digitizer/touch collection and the force-touch mouse collection) is
    // forwarded to the single InputQueue. AmtPtpEvtUsbInterruptPipeReadComplete
    // opportunistically claims a second pending request off that same queue
    // for mouse delivery, exactly as before.
    status = WdfRequestForwardToIoQueue(
        Request,
        pDevContext->InputQueue
    );

    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    if (NULL != Pending) {
        *Pending = TRUE;
    }

exit:
    return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
// Complete or acknowledge stop requests before power transition.
{
    UNREFERENCED_PARAMETER(Queue);

    // Suspend: acknowledge and let the framework requeue the request.
    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
        return;
    }

    // Purge: cancel forwarded requests directly so stop/remove can finish.
    if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
}