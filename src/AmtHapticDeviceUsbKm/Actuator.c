// Actuator.c - How a click is encoded and sent to the actuator.
//
// WHY THE REQUEST IS PREALLOCATED AND REUSED
// A click must not fail because the system is short of memory at the wrong
// moment, and it should not allocate on every press. AmtHapticPrepareTransfer
// therefore creates one request and one report buffer when the hardware is
// prepared and formats the request once: the framework allocates the IRP when
// a request is created FOR an I/O target, and the USB request context (with
// its URB) the first time a request is formatted for a control transfer -
// both are then kept for the life of the request. Every click re-formats the
// same request (WdfRequestReuse) and copies its waveform into the same buffer.
// This is safe because the sequential default queue (Queue.c) never has two
// clicks in flight, and the client's request is only completed at the very end
// of the completion routine, after the request has been made reusable - so the
// next click cannot start earlier.
//
// WHY THE TRANSFER IS ASYNCHRONOUS
// The caller's thread only waits for the click's own completion, but no
// framework thread is parked inside the driver while the USB transfer runs, and
// the transfer can be cancelled (AmtHapticCancelClick) when the device powers
// down or is removed.

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtHapticPrepareTransfer)
#endif

// Captured actuator waveforms (HID output report 0x53, 14 bytes). The bytes
// are the actuator's own protocol, not something this driver can derive; they
// follow the t2linux project's t2_trackpad_actuator payloads. Do not edit a
// value without testing it on the hardware.
static const UCHAR AmtHapticReports[AMT_HAPTIC_WAVEFORM_COUNT][AMT_HAPTIC_REPORT_LENGTH] =
{
    // AMT_HAPTIC_WAVEFORM_LIGHT
    { 0x53, 0x01, 0x1C, 0x8F, 0x01, 0x03, 0x00, 0x53, 0x37, 0x02, 0x02, 0x00, 0x18, 0x4D },

    // AMT_HAPTIC_WAVEFORM_FIRM
    { 0x53, 0x01, 0x2D, 0x8F, 0x01, 0x05, 0x00, 0x53, 0x37, 0x02, 0x02, 0x00, 0x30, 0x4D },
};

// Product IDs of the trackpad's composite USB device on which the actuator is
// known to be interface 3. Add one only after confirming that model really
// exposes the actuator there and accepts these reports, and add the matching
// hardware ID to the INF.
static const USHORT AmtHapticSupportedProductIds[] =
{
    0x0340, // MacBookPro16,1 (2019 16-inch)
};

static EVT_WDF_REQUEST_COMPLETION_ROUTINE AmtHapticEvtTransferComplete;

BOOLEAN
AmtHapticIsSupportedDevice(
    _In_ USHORT VendorId,
    _In_ USHORT ProductId
)
{
    ULONG i;

    if (VendorId != AMT_HAPTIC_VENDOR_ID) {
        return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(AmtHapticSupportedProductIds); i++) {
        if (AmtHapticSupportedProductIds[i] == ProductId) {
            return TRUE;
        }
    }

    return FALSE;
}

// Formats the reused request as the HID class SET_REPORT(Output, report ID)
// control transfer that carries the report buffer, addressed to the interface
// the device was validated to be (never a hard-coded number here).
static
NTSTATUS
AmtHapticFormatTransfer(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    WDF_USB_CONTROL_SETUP_PACKET setup;

    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(
        &setup,
        BmRequestHostToDevice,
        BmRequestToInterface,
        AMT_HID_REQUEST_SET_REPORT,
        AMT_HID_REPORT_VALUE(AMT_HID_REPORT_TYPE_OUTPUT, AMT_HAPTIC_REPORT_ID),
        DeviceContext->InterfaceNumber);

    return WdfUsbTargetDeviceFormatRequestForControlTransfer(
        DeviceContext->UsbDevice,
        DeviceContext->TransferRequest,
        &setup,
        DeviceContext->ReportMemory,
        NULL);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticPrepareTransfer(
    _In_ WDFDEVICE Device
)
{
    PDEVICE_CONTEXT       context = DeviceGetContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS              status;

    PAGED_CODE();

    // Both objects are parented to the device, so the framework releases them
    // with it; there is no cleanup callback to write. Each is created only
    // once: this runs on every start, and a start that failed half-way must
    // not leak (or re-create) what the previous attempt already made.
    if (context->TransferRequest == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;

        // Created FOR the USB target, so the IRP is allocated here, with the
        // stack size that target needs.
        status = WdfRequestCreate(
            &attributes,
            WdfUsbTargetDeviceGetIoTarget(context->UsbDevice),
            &context->TransferRequest);

        if (!NT_SUCCESS(status)) {
            context->TransferRequest = NULL;
            return status;
        }
    }

    if (context->ReportMemory == NULL) {
        PVOID buffer;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;

        status = WdfMemoryCreate(
            &attributes,
            NonPagedPoolNx,
            AMT_HAPTIC_POOL_TAG,
            AMT_HAPTIC_REPORT_LENGTH,
            &context->ReportMemory,
            &buffer);

        if (!NT_SUCCESS(status)) {
            context->ReportMemory = NULL;
            return status;
        }

        RtlZeroMemory(buffer, AMT_HAPTIC_REPORT_LENGTH);
    }

    // Format once now, without sending: this is where the framework allocates
    // the request's USB context and URB. The request is then marked for the
    // lazy WdfRequestReuse every click path already knows how to do.
    status = AmtHapticFormatTransfer(context);
    context->TransferRequestNeedsReuse = TRUE;

    return status;
}

NTSTATUS
AmtHapticStartClick(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST      Request,
    _In_ ULONG           Waveform
)
{
    WDF_REQUEST_SEND_OPTIONS     options;
    WDF_REQUEST_REUSE_PARAMS     reuse;
    PVOID                        buffer;
    NTSTATUS                     status;

    if (Waveform >= AMT_HAPTIC_WAVEFORM_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    // AmtHapticPrepareTransfer has not completed (the device never started).
    if (DeviceContext->TransferRequest == NULL || DeviceContext->ReportMemory == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (DeviceContext->TransferRequestNeedsReuse) {
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        status = WdfRequestReuse(DeviceContext->TransferRequest, &reuse);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        DeviceContext->TransferRequestNeedsReuse = FALSE;
    }

    buffer = WdfMemoryGetBuffer(DeviceContext->ReportMemory, NULL);
    RtlCopyMemory(buffer, AmtHapticReports[Waveform], AMT_HAPTIC_REPORT_LENGTH);

    status = AmtHapticFormatTransfer(DeviceContext);

    if (!NT_SUCCESS(status)) {
        DeviceContext->TransferRequestNeedsReuse = TRUE;
        return status;
    }

    WdfRequestSetCompletionRoutine(
        DeviceContext->TransferRequest,
        AmtHapticEvtTransferComplete,
        Request);

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(AMT_HAPTIC_TRANSFER_TIMEOUT_MS));

    if (!WdfRequestSend(
            DeviceContext->TransferRequest,
            WdfUsbTargetDeviceGetIoTarget(DeviceContext->UsbDevice),
            &options)) {

        // The completion routine is not called when the send itself fails, so
        // the request is reset here, lazily, before its next use.
        status = WdfRequestGetStatus(DeviceContext->TransferRequest);
        DeviceContext->TransferRequestNeedsReuse = TRUE;

        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    // From here on the completion routine owns Request; do not touch it.
    return STATUS_PENDING;
}

VOID
AmtHapticCancelClick(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    if (DeviceContext->TransferRequest != NULL) {
        (VOID)WdfRequestCancelSentRequest(DeviceContext->TransferRequest);
    }
}

static
VOID
AmtHapticEvtTransferComplete(
    _In_ WDFREQUEST                     TransferRequest,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
)
// Runs when the control transfer finishes, fails, times out, or is cancelled.
// IRQL <= DISPATCH_LEVEL.
{
    WDFREQUEST                          clientRequest = (WDFREQUEST)Context;
    PDEVICE_CONTEXT                     deviceContext = DeviceGetContext(WdfIoTargetGetDevice(Target));
    PWDF_USB_REQUEST_COMPLETION_PARAMS  usbParams = Params->Parameters.Usb.Completion;
    WDF_REQUEST_REUSE_PARAMS            reuse;
    NTSTATUS                            status = Params->IoStatus.Status;

    // The transferred length comes from the USB-specific completion
    // parameters of a control transfer, the one place that reports it
    // reliably for a request formatted by WdfUsbTargetDeviceFormatRequestFor-
    // ControlTransfer. A short transfer means the device took only part of the
    // report: not a click that was felt.
    if (NT_SUCCESS(status) &&
        usbParams != NULL &&
        usbParams->Type == WdfUsbRequestTypeDeviceControlTransfer &&
        usbParams->Parameters.DeviceControlTransfer.Length != AMT_HAPTIC_REPORT_LENGTH) {

        status = STATUS_DEVICE_DATA_ERROR;
    }

    InterlockedExchange(&deviceContext->LastStatus, status);
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&deviceContext->ClicksDelivered);
    } else {
        InterlockedIncrement(&deviceContext->ClicksFailed);
    }

    AmtHapticTrace(deviceContext, "click transfer finished, status=0x%08X\n", status);

    // Make the request reusable BEFORE completing the client's request: the
    // sequential queue starts the next click only after that completion, so no
    // other thread can be using the request while it is reset here.
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    deviceContext->TransferRequestNeedsReuse = !NT_SUCCESS(WdfRequestReuse(TransferRequest, &reuse));

    WdfRequestComplete(clientRequest, status);
}
