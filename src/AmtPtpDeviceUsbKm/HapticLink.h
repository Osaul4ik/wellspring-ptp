// HapticLink.h - link from the touchpad driver to the haptic actuator driver.
//
// AmtHapticDeviceUsbKm (a separate function driver on the trackpad's MI_03
// interface) exposes a device interface and an IOCTL that plays one click
// waveform. This file lets the TOUCHPAD driver call it directly, at the exact
// moment a hard press becomes a Force Tap (see Interrupt.c), so the second
// haptic click works without the Control Center running.
//
// The two drivers stay independent: if the actuator driver is not installed
// the link simply never opens and AmtHapticLinkClick() does nothing.
//
// The wire format below is a MIRROR of AmtHapticDeviceUsbKm/Public.h (the
// device-interface GUID is defined in HapticLink.c). Keep them in sync and
// bump AMT_HAPTIC_INTERFACE_VERSION on both sides on any layout change.

#pragma once

#define AMT_HAPTIC_INTERFACE_VERSION 1

typedef struct _AMT_HAPTIC_CLICK_REQUEST
{
    ULONG StructVersion;   // AMT_HAPTIC_INTERFACE_VERSION
    ULONG Waveform;        // AMT_POINTER_HAPTIC_WAVEFORM_* (same values as the actuator's)
} AMT_HAPTIC_CLICK_REQUEST, *PAMT_HAPTIC_CLICK_REQUEST;

C_ASSERT(sizeof(AMT_HAPTIC_CLICK_REQUEST) == 8);

#define AMT_HAPTIC_IOCTL_INDEX 0x800

// Input: AMT_HAPTIC_CLICK_REQUEST. Fires one click.
#define IOCTL_AMT_HAPTIC_CLICK \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_HAPTIC_IOCTL_INDEX + 1, METHOD_BUFFERED, FILE_WRITE_DATA)

EXTERN_C_START

// Called once from CreateDevice. Creates the link's lock and queues a work
// item that registers for the actuator's device-interface arrival/removal
// (the registration is deferred to a work item so no PnP-time opens happen
// inside AddDevice). Failure is not fatal: the touchpad works without haptics.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticLinkStart(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
);

// Called from the device's context cleanup. Stops the registration, closes
// the I/O target (cancelling any click in flight). Safe if Start never ran.
_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtHapticLinkStop(
    _Inout_ PDEVICE_CONTEXT DeviceContext
);

// Fires one click, best-effort and non-blocking. Callable at IRQL <=
// DISPATCH_LEVEL (frame-processing path). Does nothing if the actuator
// driver is not present or the previous click is still in flight.
VOID
AmtHapticLinkClick(
    _Inout_ PDEVICE_CONTEXT DeviceContext,
    _In_    ULONG           Waveform
);

EXTERN_C_END
