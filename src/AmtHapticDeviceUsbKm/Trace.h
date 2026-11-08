// Trace.h - Runtime-switchable kernel debug tracing.
//
// Same mechanism as AmtPtpDeviceUsbKm's Trace.h, so one habit works for both
// drivers: output is gated by DEVICE_CONTEXT::TraceDebugEnabled, loaded once per
// device from the "DebugMode" REG_DWORD under this device's "Device Parameters"
// key (set by the INF to 0).
//
//   DebugMode absent, unreadable, or 0 (shipped default): AmtHapticTrace()
//   costs one pointer/bool check and nothing is formatted or evaluated.
//
//   DebugMode = 1: every call fires a DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...) line
//   prefixed "[AmtHaptic] ", so DebugView/WinDbg filters can tell it apart from
//   "[AmtPtp] " output of the touchpad driver.
//
// Deliberately a plain boolean, not WPP: no preprocessing step, and a developer
// can flip it per device with regedit.
//
// Driver.h includes Device.h before this header, so DEVICE_CONTEXT is defined
// wherever AmtHapticTrace() is used.

#pragma once

EXTERN_C_START

#define AMT_HAPTIC_TRACE_DEBUGMODE_VALUE_NAME L"DebugMode"

// Also filterable independently of other drivers via the standard "Debug Print
// Filter" registry mechanism, on top of this driver's own DebugMode switch.
#define AMT_HAPTIC_TRACE_DPFLTR_ID DPFLTR_IHVDRIVER_ID

// Callers go through AmtHapticTrace() below so the DebugMode check happens
// first. DbgPrintEx is safe up to DISPATCH_LEVEL, which the transfer completion
// routine can run at.
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticTracePrint(
    _In_ PCSTR Format,
    ...
);

// Macro rather than an inline function so the DebugMode check short-circuits
// BEFORE any variadic argument is evaluated when tracing is off. Ctx may be
// NULL (treated as disabled; never dereferenced).
#define AmtHapticTrace(Ctx, Format, ...)                                    \
    do {                                                                    \
        PDEVICE_CONTEXT _amtTraceCtx = (Ctx);                               \
        if (_amtTraceCtx != NULL && _amtTraceCtx->TraceDebugEnabled) {      \
            AmtHapticTracePrint((Format), ##__VA_ARGS__);                   \
        }                                                                   \
    } while (0)

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtHapticTraceLoadDebugModeFromRegistry(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
);

EXTERN_C_END
