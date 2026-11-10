// HapticLink.c - see HapticLink.h.

#include "Driver.h"

// The actuator driver's contract: device-interface class GUID, IOCTL codes
// and request structure. Its own Public.h is included (rather than mirrored
// by hand) so the two drivers cannot drift apart; <initguid.h> first makes
// DEFINE_GUID emit the GUID's storage here, so no import library is needed.
// wdmguid.h supplies GUID_DEVICE_INTERFACE_ARRIVAL / _REMOVAL the same way.
#include <initguid.h>
#include <wdmguid.h>
#include "../AmtHapticDeviceUsbKm/Public.h"

// The pointer config stores the waveform with its own names (Public.h of this
// driver) and AmtHapticLinkClick passes that value straight through.
C_ASSERT(AMT_POINTER_HAPTIC_WAVEFORM_LIGHT == AMT_HAPTIC_WAVEFORM_LIGHT);
C_ASSERT(AMT_POINTER_HAPTIC_WAVEFORM_FIRM  == AMT_HAPTIC_WAVEFORM_FIRM);
C_ASSERT(AMT_POINTER_HAPTIC_WAVEFORM_MAX   == AMT_HAPTIC_WAVEFORM_COUNT - 1);

// A click that the actuator has not completed within this time is cancelled so
// a wedged actuator can never block later clicks forever.
#define AMT_HAPTIC_LINK_SEND_TIMEOUT_MS 250

#define AMT_HAPTIC_LINK_TAG 'lpAH'

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtHapticLinkStart)
#pragma alloc_text (PAGE, AmtHapticLinkStop)
#endif

// ---------------------------------------------------------------------------
// Locking model
//
// HapticLinkLock (spin lock, DISPATCH_LEVEL-safe) guards the four pointers
// HapticTarget / HapticRequest / HapticMemory / HapticBuffer and
// HapticLinkState. The send path (AmtHapticLinkClick) holds it across
// Reuse + Format + Send, and every path that closes the target first moves
// the state away from READY (or takes the pointers out) under the same lock -
// so a sender can never use a target that is being closed or deleted.
//
// The request completion routine takes NO lock (it only updates Interlocked
// counters, makes the one Interlocked READY -> INCOMPATIBLE transition and
// clears HapticSendBusy), so it can safely run inline inside WdfRequestSend,
// i.e. with HapticLinkLock held by the sender.
//
// Link states (AMT_HAPTIC_LINK_STATE_*, Public.h):
//
//   ABSENT        no target. Entered by AmtHapticLinkClose.
//   READY         target open, clicks are sent. Entered by AmtHapticLinkOpen
//                 and by a cancelled query-remove whose re-open worked.
//   CLOSED        target object still published but closed: the actuator is
//                 being (query-)removed, or the re-open after a cancelled
//                 query-remove failed. A later interface arrival replaces it.
//   INCOMPATIBLE  target open but the actuator driver answered
//                 STATUS_REVISION_MISMATCH: no point in sending more. Left
//                 only when the interface goes away (driver update).
// ---------------------------------------------------------------------------

static
VOID
AmtHapticLinkClose(
    _Inout_ PDEVICE_CONTEXT Ctx
)
{
    WDFIOTARGET target;

    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    target = Ctx->HapticTarget;
    Ctx->HapticTarget  = NULL;
    Ctx->HapticRequest = NULL;
    Ctx->HapticMemory  = NULL;
    Ctx->HapticBuffer  = NULL;
    InterlockedExchange(&Ctx->HapticLinkState, AMT_HAPTIC_LINK_STATE_ABSENT);
    WdfSpinLockRelease(Ctx->HapticLinkLock);

    if (target != NULL) {
        // Close cancels and waits for any request still in flight; deleting
        // the target also deletes the request and memory parented to it.
        WdfIoTargetClose(target);
        WdfObjectDelete(target);

        AmtTrace(Ctx, "HapticLink: closed");
    }
}

// Moves a published target out of READY before it is closed, so no new click
// is started on it. Only acts if Target is the one currently published.
static
VOID
AmtHapticLinkMarkClosed(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_    WDFIOTARGET     Target
)
{
    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    if (Ctx->HapticTarget == Target) {
        InterlockedExchange(&Ctx->HapticLinkState, AMT_HAPTIC_LINK_STATE_CLOSED);
    }
    WdfSpinLockRelease(Ctx->HapticLinkLock);
}

// Standard remote-I/O-target removal handling: without these the actuator's
// node could not be removed while we hold it open (and, after a surprise
// removal, could not come back under the same instance path).
static EVT_WDF_IO_TARGET_QUERY_REMOVE    AmtHapticLinkEvtTargetQueryRemove;
static EVT_WDF_IO_TARGET_REMOVE_CANCELED AmtHapticLinkEvtTargetRemoveCanceled;
static EVT_WDF_IO_TARGET_REMOVE_COMPLETE AmtHapticLinkEvtTargetRemoveComplete;

static
NTSTATUS
AmtHapticLinkEvtTargetQueryRemove(
    _In_ WDFIOTARGET IoTarget
)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoTargetGetDevice(IoTarget));

    AmtHapticLinkMarkClosed(ctx, IoTarget);
    WdfIoTargetCloseForQueryRemove(IoTarget);
    return STATUS_SUCCESS;
}

static
VOID
AmtHapticLinkEvtTargetRemoveCanceled(
    _In_ WDFIOTARGET IoTarget
)
{
    PDEVICE_CONTEXT           ctx = DeviceGetContext(WdfIoTargetGetDevice(IoTarget));
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    NTSTATUS                  status;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_REOPEN(&openParams);
    status = WdfIoTargetOpen(IoTarget, &openParams);

    if (!NT_SUCCESS(status)) {
        // Stays CLOSED: the frame path then behaves as if the haptic click
        // were off, instead of arbitrating presses for a click that cannot
        // be played. The next interface arrival replaces this target.
        AmtTrace(ctx, "HapticLink: re-open after cancelled removal FAILED, status=0x%08X", status);
        return;
    }

    WdfSpinLockAcquire(ctx->HapticLinkLock);
    if (ctx->HapticTarget == IoTarget) {
        InterlockedExchange(&ctx->HapticLinkState, AMT_HAPTIC_LINK_STATE_READY);
    }
    WdfSpinLockRelease(ctx->HapticLinkLock);
}

static
VOID
AmtHapticLinkEvtTargetRemoveComplete(
    _In_ WDFIOTARGET IoTarget
)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoTargetGetDevice(IoTarget));

    // Also the first (and only) callback of a surprise removal, which has no
    // query-remove before it. The target object itself is deleted by the
    // interface-removal notification (AmtHapticLinkClose) or replaced by the
    // next arrival; it must not be deleted from inside its own callback.
    AmtHapticLinkMarkClosed(ctx, IoTarget);
    WdfIoTargetClose(IoTarget);
}

static
VOID
AmtHapticLinkEvtRequestComplete(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
)
// IRQL <= DISPATCH_LEVEL; may run inline inside WdfRequestSend, with
// HapticLinkLock held by AmtHapticLinkClick. Takes no lock.
{
    PDEVICE_CONTEXT ctx    = (PDEVICE_CONTEXT)Context;
    NTSTATUS        status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    InterlockedExchange(&ctx->HapticLastStatus, status);

    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&ctx->HapticClicksDelivered);
    } else if (status == STATUS_DEVICE_BUSY) {
        // The actuator's own minimum-interval limit: flow control, not a fault.
        InterlockedIncrement(&ctx->HapticClicksThrottled);
    } else {
        InterlockedIncrement(&ctx->HapticClicksFailed);

        if (status == STATUS_REVISION_MISMATCH) {
            // The actuator driver was built from another interface revision
            // and will refuse every click. Stop asking, so the frame path
            // stops arbitrating presses for a click that never comes.
            (VOID)InterlockedCompareExchange(
                &ctx->HapticLinkState,
                AMT_HAPTIC_LINK_STATE_INCOMPATIBLE,
                AMT_HAPTIC_LINK_STATE_READY);
        }
    }

    // Last: from here on the request may be reused by the next click.
    InterlockedExchange(&ctx->HapticSendBusy, 0);
}

_IRQL_requires_(PASSIVE_LEVEL)
static
VOID
AmtHapticLinkOpen(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_    PCUNICODE_STRING SymbolicLink
)
{
    WDF_OBJECT_ATTRIBUTES      attributes;
    WDF_IO_TARGET_OPEN_PARAMS  openParams;
    WDFIOTARGET                target  = NULL;
    WDFREQUEST                 request = NULL;
    WDFMEMORY                  memory  = NULL;
    PAMT_HAPTIC_CLICK_REQUEST  buffer  = NULL;
    NTSTATUS                   status;
    BOOLEAN                    havePublished;
    BOOLEAN                    publishedIsStale;

    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    havePublished    = (Ctx->HapticTarget != NULL);
    publishedIsStale = havePublished &&
        (Ctx->HapticLinkState == (LONG)AMT_HAPTIC_LINK_STATE_CLOSED);
    WdfSpinLockRelease(Ctx->HapticLinkLock);

    if (havePublished && !publishedIsStale) {
        return;     // one actuator per touchpad is all there is
    }

    if (publishedIsStale) {
        // A target left closed by a completed removal (or a failed re-open)
        // whose interface-removal notification we did not get: drop it, or
        // the link would stay dead until the touchpad itself is restarted.
        AmtHapticLinkClose(Ctx);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->HapticParentDevice;
    status = WdfIoTargetCreate(Ctx->HapticParentDevice, &attributes, &target);
    if (!NT_SUCCESS(status)) {
        AmtTrace(Ctx, "HapticLink: WdfIoTargetCreate FAILED, status=0x%08X", status);
        return;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams, (PUNICODE_STRING)SymbolicLink, GENERIC_WRITE);
    // Other openers (the Control Center's test click) must be able to share it.
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    openParams.EvtIoTargetQueryRemove    = AmtHapticLinkEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = AmtHapticLinkEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = AmtHapticLinkEvtTargetRemoveComplete;

    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        AmtTrace(Ctx, "HapticLink: WdfIoTargetOpen FAILED, status=0x%08X", status);
        WdfObjectDelete(target);
        return;
    }

    // Request and its input buffer are created once and reused for every
    // click; both are children of the target, so they go away with it.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = target;
    status = WdfRequestCreate(&attributes, target, &request);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = request;
    status = WdfMemoryCreate(
        &attributes, NonPagedPoolNx, AMT_HAPTIC_LINK_TAG,
        sizeof(AMT_HAPTIC_CLICK_REQUEST), &memory, (PVOID*)&buffer);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    buffer->StructVersion = AMT_HAPTIC_INTERFACE_VERSION;
    buffer->Waveform      = AMT_HAPTIC_WAVEFORM_LIGHT;

    // Format once now, at PASSIVE_LEVEL and without sending: the framework
    // allocates a request's I/O context and its METHOD_BUFFERED system buffer
    // the first time it is formatted and keeps both afterwards, so the click
    // path (DISPATCH_LEVEL, frame processing) never allocates. Every click
    // starts with WdfRequestReuse, which also resets this priming format.
    status = WdfIoTargetFormatRequestForIoctl(
        target, request, IOCTL_AMT_HAPTIC_CLICK, memory, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    InterlockedExchange(&Ctx->HapticSendBusy, 0);
    Ctx->HapticRequest = request;
    Ctx->HapticMemory  = memory;
    Ctx->HapticBuffer  = buffer;
    Ctx->HapticTarget  = target;
    // Published last: READY is what lets a click through.
    InterlockedExchange(&Ctx->HapticLinkState, AMT_HAPTIC_LINK_STATE_READY);
    WdfSpinLockRelease(Ctx->HapticLinkLock);

    AmtTrace(Ctx, "HapticLink: opened");
    return;

fail:
    AmtTrace(Ctx, "HapticLink: request setup FAILED, status=0x%08X", status);
    WdfIoTargetClose(target);
    WdfObjectDelete(target);
}

// Runs at PASSIVE_LEVEL (documented behaviour of device-interface change
// notifications), including for the interfaces that already exist when the
// registration is made (PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES).
static
NTSTATUS
AmtHapticLinkNotify(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION notification =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    if (ctx == NULL || notification == NULL) {
        return STATUS_SUCCESS;
    }

    if (IsEqualGUID(&notification->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) {
        AmtHapticLinkOpen(ctx, notification->SymbolicLinkName);
    } else if (IsEqualGUID(&notification->Event, &GUID_DEVICE_INTERFACE_REMOVAL)) {
        AmtHapticLinkClose(ctx);
    }

    return STATUS_SUCCESS;
}

// Deferred registration - see AmtHapticLinkStart.
static
VOID
AmtHapticLinkEvtStartWorkItem(
    _In_ WDFWORKITEM WorkItem
)
{
    WDFDEVICE       device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PDEVICE_CONTEXT ctx    = DeviceGetContext(device);
    PVOID           entry  = NULL;
    NTSTATUS        status;

    status = IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_AMT_HAPTIC,
        WdfDriverWdmGetDriverObject(WdfGetDriver()),
        AmtHapticLinkNotify,
        ctx,
        &entry);

    if (NT_SUCCESS(status)) {
        ctx->HapticNotifyEntry = entry;
    } else {
        AmtTrace(ctx, "HapticLink: IoRegisterPlugPlayNotification FAILED, status=0x%08X", status);
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticLinkStart(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG   workConfig;
    NTSTATUS              status;

    PAGED_CODE();

    DeviceContext->HapticParentDevice = Device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfSpinLockCreate(&attributes, &DeviceContext->HapticLinkLock);
    if (!NT_SUCCESS(status)) {
        DeviceContext->HapticLinkLock = NULL;
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&workConfig, AmtHapticLinkEvtStartWorkItem);
    workConfig.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWorkItemCreate(&workConfig, &attributes, &DeviceContext->HapticStartWorkItem);
    if (!NT_SUCCESS(status)) {
        DeviceContext->HapticStartWorkItem = NULL;
        return status;
    }

    WdfWorkItemEnqueue(DeviceContext->HapticStartWorkItem);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtHapticLinkStop(
    _Inout_ PDEVICE_CONTEXT DeviceContext
)
{
    PAGED_CODE();

    if (DeviceContext->HapticLinkLock == NULL) {
        return;     // Start never got far enough to need any teardown
    }

    // 1. Let a still-pending registration finish, so it cannot register
    //    AFTER the unregister below.
    if (DeviceContext->HapticStartWorkItem != NULL) {
        WdfWorkItemFlush(DeviceContext->HapticStartWorkItem);
    }

    // 2. Stop notifications. This waits for a callback that is running, so
    //    it must not be called with HapticLinkLock held.
    if (DeviceContext->HapticNotifyEntry != NULL) {
        (VOID)IoUnregisterPlugPlayNotificationEx(DeviceContext->HapticNotifyEntry);
        DeviceContext->HapticNotifyEntry = NULL;
    }

    // 3. Drop the target (cancels a click in flight).
    AmtHapticLinkClose(DeviceContext);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticLinkClick(
    _Inout_ PDEVICE_CONTEXT DeviceContext,
    _In_    ULONG           Waveform
)
{
    WDF_REQUEST_REUSE_PARAMS  reuse;
    WDF_REQUEST_SEND_OPTIONS  sendOptions;
    NTSTATUS                  status;

    InterlockedIncrement(&DeviceContext->HapticClicksRequested);

    // Lock-free early out: no actuator (the common case on every model the
    // actuator driver does not bind to) must cost nothing on the frame path.
    if (DeviceContext->HapticLinkLock == NULL ||
        !AmtHapticLinkIsReady(DeviceContext)) {
        InterlockedIncrement(&DeviceContext->HapticClicksDropped);
        return;
    }

    WdfSpinLockAcquire(DeviceContext->HapticLinkLock);

    if (DeviceContext->HapticTarget == NULL ||
        !AmtHapticLinkIsReady(DeviceContext) ||
        InterlockedCompareExchange(&DeviceContext->HapticSendBusy, 1, 0) != 0) {
        // The link went away since the check above, or the previous click
        // has not completed yet.
        WdfSpinLockRelease(DeviceContext->HapticLinkLock);
        InterlockedIncrement(&DeviceContext->HapticClicksDropped);
        return;
    }

    ((PAMT_HAPTIC_CLICK_REQUEST)DeviceContext->HapticBuffer)->Waveform = Waveform;

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(DeviceContext->HapticRequest, &reuse);

    if (NT_SUCCESS(status)) {
        status = WdfIoTargetFormatRequestForIoctl(
            DeviceContext->HapticTarget,
            DeviceContext->HapticRequest,
            IOCTL_AMT_HAPTIC_CLICK,
            DeviceContext->HapticMemory, NULL,
            NULL, NULL);
    }

    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(
            DeviceContext->HapticRequest,
            AmtHapticLinkEvtRequestComplete,
            DeviceContext);

        WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
            &sendOptions, WDF_REL_TIMEOUT_IN_MS(AMT_HAPTIC_LINK_SEND_TIMEOUT_MS));

        if (WdfRequestSend(DeviceContext->HapticRequest,
                           DeviceContext->HapticTarget, &sendOptions)) {
            // Under way: the completion routine accounts for it and clears
            // HapticSendBusy (it may already have run, inline).
            WdfSpinLockRelease(DeviceContext->HapticLinkLock);
            return;
        }

        // Not sent (target closed / being removed): the completion routine
        // will not run for it.
        status = WdfRequestGetStatus(DeviceContext->HapticRequest);
        if (NT_SUCCESS(status)) {
            status = STATUS_UNSUCCESSFUL;
        }
    }

    InterlockedExchange(&DeviceContext->HapticLastStatus, status);
    InterlockedIncrement(&DeviceContext->HapticClicksFailed);
    InterlockedExchange(&DeviceContext->HapticSendBusy, 0);

    WdfSpinLockRelease(DeviceContext->HapticLinkLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticLinkQueryInfo(
    _In_  PDEVICE_CONTEXT       DeviceContext,
    _Out_ PAMT_HAPTIC_LINK_INFO Info
)
{
    RtlZeroMemory(Info, sizeof(*Info));

    // Each value is read atomically; the set is a diagnostic snapshot and is
    // not required to be mutually consistent (a click can complete between
    // two reads).
    Info->StructVersion   = AMT_HAPTIC_LINK_INFO_VERSION;
    Info->State           = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticLinkState, 0, 0);
    Info->ClicksRequested = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticClicksRequested, 0, 0);
    Info->ClicksDelivered = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticClicksDelivered, 0, 0);
    Info->ClicksThrottled = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticClicksThrottled, 0, 0);
    Info->ClicksFailed    = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticClicksFailed, 0, 0);
    Info->ClicksDropped   = (ULONG)InterlockedCompareExchange(&DeviceContext->HapticClicksDropped, 0, 0);
    Info->LastStatus      = InterlockedCompareExchange(&DeviceContext->HapticLastStatus, 0, 0);
}
