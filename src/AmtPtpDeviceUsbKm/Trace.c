// Trace.c - Runtime-switchable debug tracing implementation, plus the
// power-state / IOCTL name helpers used by AmtTrace() call sites.
//
// See Trace.h for the full design rationale (why this is a DebugMode
// registry switch and not WPP, and how this differs from the GUI "Live"
// monitor). This file supersedes the old DebugUtils.c, which defined
// DbgDevicePowerString but was never wired into
// AmtPtpDeviceUsbKm.vcxproj - it was not compiled into the driver at all.

#include "Driver.h"
#include <stdarg.h> // va_list/va_start/va_end for AmtTracePrint - not
                     // reliably pulled in transitively by ntddk.h/wdm.h in
                     // every WDK header configuration (build broke with
                     // C4013 'va_start' undefined without this).

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtTraceLoadDebugModeFromRegistry)
#endif

// AmtTracePrint
//
// Formats and prints one "[AmtPtp] "-prefixed trace line via DbgPrintEx.
// Only ever reached through the AmtTrace() macro (Trace.h), which has
// already checked DEVICE_CONTEXT::TraceDebugEnabled before evaluating any
// of the variadic arguments - this function itself does no gating.
//
// vDbgPrintExWithPrefix is the documented way to add a fixed tag/prefix to
// DbgPrintEx output without building the prefix into every call site's own
// format string (Format here is a runtime PCSTR from the macro, so
// compile-time string-literal concatenation isn't an option).
//
// Deliberately NOT paged: several AmtTrace() call sites in Interrupt.c
// (the continuous-reader completion routine, EvtUsbTargetPipeReadersFailed)
// run at DISPATCH_LEVEL, and DbgPrintEx/vDbgPrintExWithPrefix are
// documented safe up to DISPATCH_LEVEL.
//
// BUG FIX: this definition was missing the _IRQL_requires_max_(DISPATCH_LEVEL)
// annotation that the declaration in Trace.h already carries, which
// PREfast flagged as an inconsistent annotation (C28252) between the two.
// Must match exactly - PREfast checks the declaration/definition pair, not
// just the declaration.
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtTracePrint(
    _In_ PCSTR Format,
    ...
)
{
    va_list args;

    va_start(args, Format);
    vDbgPrintExWithPrefix(
        "[AmtPtp] ",
        AMT_TRACE_DPFLTR_ID,
        DPFLTR_INFO_LEVEL,
        Format,
        args);
    va_end(args);
}

// AmtTraceLoadDebugModeFromRegistry

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtTraceLoadDebugModeFromRegistry(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
)
{
    WDFKEY          key;
    NTSTATUS        status;
    ULONG           value;
    UNICODE_STRING  valueName;

    PAGED_CODE();

    // Compiled-in default: tracing OFF. Same "default first, then
    // best-effort registry override" sequence as PalmConfig/PointerConfig/
    // ScrollConfig in AmtPtpDeviceUsbKmCreateDevice (Device.c) - a missing
    // or unreadable value must never leave TraceDebugEnabled in an
    // indeterminate state.
    DeviceContext->TraceDebugEnabled = FALSE;

    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return; // No Device Parameters key yet (or no access) - stay off.
    }

    RtlInitUnicodeString(&valueName, AMT_TRACE_DEBUGMODE_VALUE_NAME);
    status = WdfRegistryQueryULong(key, &valueName, &value);
    if (NT_SUCCESS(status)) {
        DeviceContext->TraceDebugEnabled = (value != 0);
    }
    // else: value absent/wrong type - stay off, no different than a fresh
    // install that has never had DebugMode written to it.

    WdfRegistryClose(key);
}

// DbgDevicePowerString
//
// Unchanged from the old (uncompiled) DebugUtils.c, aside from returning
// PCSTR instead of PCHAR (these are read-only string-literal tables; PCHAR
// let callers legally write through the return value, which was never the
// intent).

PCSTR
DbgDevicePowerString(
    _In_ WDF_POWER_DEVICE_STATE Type
)
{
    switch (Type)
    {
    case WdfPowerDeviceInvalid:
        return "WdfPowerDeviceInvalid";
    case WdfPowerDeviceD0:
        return "WdfPowerDeviceD0";
    case WdfPowerDeviceD1:
        return "WdfPowerDeviceD1";
    case WdfPowerDeviceD2:
        return "WdfPowerDeviceD2";
    case WdfPowerDeviceD3:
        return "WdfPowerDeviceD3";
    case WdfPowerDeviceD3Final:
        return "WdfPowerDeviceD3Final";
    case WdfPowerDevicePrepareForHibernation:
        return "WdfPowerDevicePrepareForHibernation";
    case WdfPowerDeviceMaximum:
        return "WdfPowerDeviceMaximum";
    default:
        return "UnknownDevicePowerState";
    }
}
