// HapticLink.c - see HapticLink.h.

#include "Driver.h"

// The actuator driver's device-interface class. Defined here (INITGUID) so no
// import library is needed; must match AmtHapticDeviceUsbKm/Public.h.
// wdmguid.h supplies GUID_DEVICE_INTERFACE_ARRIVAL / _REMOVAL the same way.
#include <initguid.h>
#include <wdmguid.h>

// {15E33C40-8EC9-4AB3-AFE1-B3E6215F4499}
DEFINE_GUID(GUID_DEVINTERFACE_AMT_HAPTIC,
    0x15e33c40, 0x8ec9, 0x4ab3, 0xaf, 0xe1, 0xb3, 0xe6, 0x21, 0x5f, 0x44, 0x99);

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
// HapticTarget / HapticRequest / HapticMemory / HapticBuffer. The send path
// (AmtHapticLinkClick) holds it across Reuse + Format + Send, and the close
// path takes the pointers out under the same lock BEFORE tearing the target
// down - so a sender can never use a target that is being deleted. The
// request completion routine takes NO lock (it only clears HapticSendBusy),
// so it can safely run inline inside WdfRequestSend.
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
    WdfSpinLockRelease(Ctx->HapticLinkLock);

    if (target != NULL) {
        // Close cancels and waits for any request still in flight; deleting
        // the target also deletes the request and memory parented to it.
        WdfIoTargetClose(target);
        WdfObjectDelete(target);
    }
}

// Standard remote-I/O-target removal handling: without these the actuator's
// node could not be removed while we hold it open.
static EVT_WDF_IO_TARGET_QUERY_REMOVE  AmtHapticLinkEvtTargetQueryRemove;
static EVT_WDF_IO_TARGET_REMOVE_CANCELED AmtHapticLinkEvtTargetRemoveCanceled;
static EVT_WDF_IO_TARGET_REMOVE_COMPLETE AmtHapticLinkEvtTargetRemoveComplete;

static
NTSTATUS
AmtHapticLinkEvtTargetQueryRemove(
    _In_ WDFIOTARGET IoTarget
)
{
    WdfIoTargetCloseForQueryRemove(IoTarget);
    return STATUS_SUCCESS;
}

static
VOID
AmtHapticLinkEvtTargetRemoveCanceled(
    _In_ WDFIOTARGET IoTarget
)
{
    WDF_IO_TARGET_OPEN_PARAMS openParams;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_REOPEN(&openParams);
    (VOID)WdfIoTargetOpen(IoTarget, &openParams);
}

static
VOID
AmtHapticLinkEvtTargetRemoveComplete(
    _In_ WDFIOTARGET IoTarget
)
{
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
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);

    // Whatever the outcome (clicked, throttled by the actuator's own
    // minimum-interval limit, failed, timed out): the request is free again.
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
    BOOLEAN                    alreadyOpen;

    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    alreadyOpen = (Ctx->HapticTarget != NULL);
    WdfSpinLockRelease(Ctx->HapticLinkLock);
    if (alreadyOpen) {
        return;     // one actuator per touchpad is all there is
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->HapticParentDevice;
    status = WdfIoTargetCreate(Ctx->HapticParentDevice, &attributes, &target);
    if (!NT_SUCCESS(status)) {
        return;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams, (PUNICODE_STRING)SymbolicLink, GENERIC_WRITE);
    // Other openers (the Control Center's test click) must be able to share it.
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    openParams.EvtIoTargetQueryRemove   = AmtHapticLinkEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = AmtHapticLinkEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = AmtHapticLinkEvtTargetRemoveComplete;

    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
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
    buffer->Waveform      = AMT_POINTER_HAPTIC_WAVEFORM_FIRM;

    WdfSpinLockAcquire(Ctx->HapticLinkLock);
    InterlockedExchange(&Ctx->HapticSendBusy, 0);
    Ctx->HapticRequest = request;
    Ctx->HapticMemory  = memory;
    Ctx->HapticBuffer  = buffer;
    Ctx->HapticTarget  = target;      // published last: this is the "open" flag
    WdfSpinLockRelease(Ctx->HapticLinkLock);
    return;

fail:
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

VOID
AmtHapticLinkClick(
    _Inout_ PDEVICE_CONTEXT DeviceContext,
    _In_    ULONG           Waveform
)
{
    WDF_REQUEST_REUSE_PARAMS  reuse;
    WDF_REQUEST_SEND_OPTIONS  sendOptions;
    NTSTATUS                  status;

    if (DeviceContext->HapticLinkLock == NULL) {
        return;
    }

    WdfSpinLockAcquire(DeviceContext->HapticLinkLock);

    if (DeviceContext->HapticTarget == NULL ||
        InterlockedCompareExchange(&DeviceContext->HapticSendBusy, 1, 0) != 0) {
        // No actuator, or the previous click has not completed yet.
        WdfSpinLockRelease(DeviceContext->HapticLinkLock);
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

        if (!WdfRequestSend(DeviceContext->HapticRequest,
                            DeviceContext->HapticTarget, &sendOptions)) {
            // Not sent (target closed / being removed): free the request now;
            // the completion routine will not run for it.
            InterlockedExchange(&DeviceContext->HapticSendBusy, 0);
        }
    } else {
        InterlockedExchange(&DeviceContext->HapticSendBusy, 0);
    }

    WdfSpinLockRelease(DeviceContext->HapticLinkLock);
}
