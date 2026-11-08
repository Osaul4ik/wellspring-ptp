// Trace.c - Runtime-switchable debug tracing implementation.
//
// See Trace.h for the design.

#include "Driver.h"
#include <stdarg.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtHapticTraceLoadDebugModeFromRegistry)
#endif

// Deliberately NOT paged: the transfer completion routine (Actuator.c) can run
// at DISPATCH_LEVEL, and vDbgPrintExWithPrefix is documented safe up to there.
// The annotation must match the declaration in Trace.h exactly - PREfast
// checks the declaration/definition pair (C28252).
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtHapticTracePrint(
    _In_ PCSTR Format,
    ...
)
{
    va_list args;

    va_start(args, Format);
    vDbgPrintExWithPrefix(
        "[AmtHaptic] ",
        AMT_HAPTIC_TRACE_DPFLTR_ID,
        DPFLTR_INFO_LEVEL,
        Format,
        args);
    va_end(args);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtHapticTraceLoadDebugModeFromRegistry(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
)
{
    WDFKEY   key;
    NTSTATUS status;
    ULONG    value = 0;
    DECLARE_CONST_UNICODE_STRING(valueName, AMT_HAPTIC_TRACE_DEBUGMODE_VALUE_NAME);

    PAGED_CODE();

    DeviceContext->TraceDebugEnabled = FALSE;

    status = WdfDeviceOpenRegistryKey(
        Device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_READ,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);

    if (!NT_SUCCESS(status)) {
        // No Device Parameters key: tracing stays off.
        return;
    }

    status = WdfRegistryQueryULong(key, &valueName, &value);
    if (NT_SUCCESS(status) && value != 0) {
        DeviceContext->TraceDebugEnabled = TRUE;
    }

    WdfRegistryClose(key);
}
