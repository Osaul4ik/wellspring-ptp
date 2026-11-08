// Actuator.h - Hardware knowledge: which devices are supported and how a
// click is encoded and sent.

#pragma once

EXTERN_C_START

#define AMT_HAPTIC_POOL_TAG 'tpHA'

// The actuator is a HID output report on one interface of the trackpad's
// composite USB device. The driver binds to that interface only, never to the
// composite parent and never to the interface the touchpad driver owns.
#define AMT_HAPTIC_VENDOR_ID        0x05AC
#define AMT_HAPTIC_INTERFACE_NUMBER 3

// HID class SET_REPORT(Output, ID 0x53). Every payload is this long.
#define AMT_HAPTIC_REPORT_ID        0x53
#define AMT_HAPTIC_REPORT_LENGTH    14

// HID class request SET_REPORT and the wValue it takes for an output report:
// report type in the high byte, report ID in the low byte.
#define AMT_HID_REQUEST_SET_REPORT       0x09
#define AMT_HID_REPORT_TYPE_OUTPUT       0x02
#define AMT_HID_REPORT_VALUE(Type, Id)   ((USHORT)(((Type) << 8) | (Id)))

// Actuator wear/heat guard: clicks closer together than this are refused.
// KeQueryInterruptTime() units are 100 ns.
#define AMT_HAPTIC_MIN_CLICK_INTERVAL_MS   100
#define AMT_HAPTIC_MIN_CLICK_INTERVAL_100NS \
    ((ULONGLONG)AMT_HAPTIC_MIN_CLICK_INTERVAL_MS * 10000ULL)

// A click is one tiny control transfer; anything slower than this is a fault.
#define AMT_HAPTIC_TRANSFER_TIMEOUT_MS     500

// TRUE for a USB device this driver is known to work on.
BOOLEAN
AmtHapticIsSupportedDevice(
    _In_ USHORT VendorId,
    _In_ USHORT ProductId
);

// Creates the request and report buffer reused for every click, so the click
// path itself allocates nothing and cannot fail for lack of memory.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticAllocateTransfer(
    _In_ WDFDEVICE Device
);

// Sends one click for Waveform and completes Request from the transfer's
// completion routine. Returns STATUS_PENDING when that is under way; any other
// status means nothing was sent and the CALLER must complete Request.
NTSTATUS
AmtHapticStartClick(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST      Request,
    _In_ ULONG           Waveform
);

// Cancels a click whose transfer is still in flight; its completion routine
// then completes the client's request. Harmless if none is.
VOID
AmtHapticCancelClick(
    _In_ PDEVICE_CONTEXT DeviceContext
);

EXTERN_C_END
