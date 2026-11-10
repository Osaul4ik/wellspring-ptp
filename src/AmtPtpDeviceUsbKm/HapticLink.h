// HapticLink.h - link from the touchpad driver to the haptic actuator driver.
//
// AmtHapticDeviceUsbKm (a separate function driver on the trackpad's MI_03
// interface) exposes a device interface and an IOCTL that plays one click
// waveform. This file lets the TOUCHPAD driver call it directly, at the exact
// moment a hard press becomes a Force Tap (see Interrupt.c), so the second
// haptic click works without the Control Center running.
//
// The two drivers stay independent: if the actuator driver is not installed
// the link simply never opens, AmtHapticLinkIsReady() stays FALSE and the
// frame path behaves as if the haptic click were switched off.
//
// The wire format (device-interface GUID, IOCTL, request structure) is NOT
// mirrored here: HapticLink.c includes the actuator driver's own Public.h, so
// there is exactly one definition of the contract in the source tree.

#pragma once

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
// DISPATCH_LEVEL (frame-processing path). Does nothing if the link is not
// ready or the previous click is still in flight; either way the outcome is
// counted (see AmtHapticLinkQueryInfo).
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticLinkClick(
    _Inout_ PDEVICE_CONTEXT DeviceContext,
    _In_    ULONG           Waveform
);

// Snapshot of the link state and its click counters for
// IOCTL_AMT_PTP_GET_HAPTIC_LINK_INFO. Fills every field of *Info.
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticLinkQueryInfo(
    _In_  PDEVICE_CONTEXT       DeviceContext,
    _Out_ PAMT_HAPTIC_LINK_INFO Info
);

// TRUE while a click can actually be delivered: the actuator driver's
// interface is open and it speaks our interface version. Lock-free (one
// volatile read), so the frame path can ask on every frame. The answer can
// be stale by the time it is used; AmtHapticLinkClick re-checks under its
// lock, so the worst case is one click that is not played.
static __forceinline BOOLEAN
AmtHapticLinkIsReady(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    // HapticLinkState is a volatile LONG: a plain read cannot tear.
    return (BOOLEAN)(DeviceContext->HapticLinkState ==
                     (LONG)AMT_HAPTIC_LINK_STATE_READY);
}

EXTERN_C_END
