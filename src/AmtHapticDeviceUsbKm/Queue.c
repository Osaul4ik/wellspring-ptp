// Queue.c - Queue entry points and callbacks.
//
// One default queue, sequential and power managed:
//   * Sequential: one click at a time, which is what lets the reused transfer
//     request (Actuator.c) and the unlocked LastClickTime work. A click is a
//     tiny transfer, so waiting behind another one costs nothing real.
//   * Power managed: requests are held while the device is not in D0, so a
//     click never reaches a suspended device.
//
// EvtIoStop: the framework calls it for a request this driver is still holding
// when the device leaves D0 or is removed. The only request that can be held is
// a click whose transfer is in flight.
//   * Suspend: acknowledge without requeue. The click is not repeated, and the
//     completion routine still completes it when the transfer ends.
//   * Purge (removal): cancel the transfer; the completion routine completes the
//     client's request with the cancellation status. Nothing here may touch
//     Request afterwards - the completion routine can already have completed it.

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtHapticQueueInitialize)
#endif

static
VOID
AmtHapticHandleGetInfo(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST      Request
)
{
    PAMT_HAPTIC_INFO info;
    NTSTATUS         status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(AMT_HAPTIC_INFO), (PVOID*)&info, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    RtlZeroMemory(info, sizeof(*info));
    info->StructVersion      = AMT_HAPTIC_INTERFACE_VERSION;
    info->WaveformCount      = AMT_HAPTIC_WAVEFORM_COUNT;
    info->MinClickIntervalMs = AMT_HAPTIC_MIN_CLICK_INTERVAL_MS;
    info->ClicksDelivered    = (ULONG)InterlockedCompareExchange(&DeviceContext->ClicksDelivered, 0, 0);
    info->ClicksThrottled    = (ULONG)InterlockedCompareExchange(&DeviceContext->ClicksThrottled, 0, 0);
    info->ClicksFailed       = (ULONG)InterlockedCompareExchange(&DeviceContext->ClicksFailed, 0, 0);
    info->LastStatus         = InterlockedCompareExchange(&DeviceContext->LastStatus, 0, 0);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(AMT_HAPTIC_INFO));
}

static
VOID
AmtHapticHandleClick(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST      Request
)
{
    PAMT_HAPTIC_CLICK_REQUEST click;
    ULONGLONG                 now;
    NTSTATUS                  status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(AMT_HAPTIC_CLICK_REQUEST), (PVOID*)&click, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    if (click->StructVersion != AMT_HAPTIC_INTERFACE_VERSION) {
        WdfRequestComplete(Request, STATUS_REVISION_MISMATCH);
        return;
    }

    if (click->Waveform >= AMT_HAPTIC_WAVEFORM_COUNT) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (DeviceContext->UsbDevice == NULL) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    now = KeQueryInterruptTime();
    if (DeviceContext->LastClickTime != 0 &&
        (now - DeviceContext->LastClickTime) < AMT_HAPTIC_MIN_CLICK_INTERVAL_100NS) {

        InterlockedIncrement(&DeviceContext->ClicksThrottled);
        WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
        return;
    }

    // Recorded BEFORE the send: once it is under way the completion can run on
    // another thread and the next click can arrive immediately.
    DeviceContext->LastClickTime = now;

    status = AmtHapticStartClick(DeviceContext, Request, click->Waveform);
    if (status != STATUS_PENDING) {
        InterlockedIncrement(&DeviceContext->ClicksFailed);
        InterlockedExchange(&DeviceContext->LastStatus, status);
        WdfRequestComplete(Request, status);
    }

    // STATUS_PENDING: the completion routine owns Request now.
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticQueueInitialize(
    _In_ WDFDEVICE Device
)
{
    WDF_IO_QUEUE_CONFIG config;
    WDFQUEUE            queue;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config, WdfIoQueueDispatchSequential);
    config.PowerManaged        = WdfTrue;
    config.EvtIoDeviceControl  = AmtHapticEvtIoDeviceControl;
    config.EvtIoStop           = AmtHapticEvtIoStop;

    return WdfIoQueueCreate(Device, &config, WDF_NO_OBJECT_ATTRIBUTES, &queue);
}

VOID
AmtHapticEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    PDEVICE_CONTEXT deviceContext = DeviceGetContext(WdfIoQueueGetDevice(Queue));

    // The buffer sizes are validated by WdfRequestRetrieve*Buffer in the
    // handlers, against the structures they actually need.
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_AMT_HAPTIC_GET_INFO:
        AmtHapticHandleGetInfo(deviceContext, Request);
        break;

    case IOCTL_AMT_HAPTIC_CLICK:
        AmtHapticHandleClick(deviceContext, Request);
        break;

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}

VOID
AmtHapticEvtIoStop(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG      ActionFlags
)
{
    PDEVICE_CONTEXT deviceContext = DeviceGetContext(WdfIoQueueGetDevice(Queue));

    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
    } else if (ActionFlags & WdfRequestStopActionPurge) {
        AmtHapticCancelClick(deviceContext);
    }
}
