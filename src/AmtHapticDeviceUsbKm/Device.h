// Device.h - Per-device state and device-level entry points.

#pragma once

EXTERN_C_START

typedef struct _DEVICE_CONTEXT
{
    // Set up by AmtHapticEvtDevicePrepareHardware.
    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    UCHAR           InterfaceNumber;

    // Reused for every click (Actuator.c); created by AmtHapticPrepareTransfer
    // on the first start, NULL before that. TransferRequestNeedsReuse is set
    // when the request has to be reset before its next use, i.e. after the
    // priming format in AmtHapticPrepareTransfer, a send that failed
    // synchronously or a reset that failed in the completion routine.
    WDFREQUEST      TransferRequest;
    WDFMEMORY       ReportMemory;
    BOOLEAN         TransferRequestNeedsReuse;

    // From the "DebugMode" registry value; see Trace.h.
    BOOLEAN         TraceDebugEnabled;

    // KeQueryInterruptTime() of the last accepted click. Only touched from the
    // sequential queue's callback, so it needs no lock.
    ULONGLONG       LastClickTime;

    // Diagnostics counters, reported by IOCTL_AMT_HAPTIC_GET_INFO.
    volatile LONG   ClicksDelivered;
    volatile LONG   ClicksThrottled;
    volatile LONG   ClicksFailed;
    volatile LONG   LastStatus;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS
AmtHapticCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
);

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtHapticEvtDevicePrepareHardware;

EXTERN_C_END
