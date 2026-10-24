// Trace.h - Centralized, runtime-switchable kernel debug tracing.
//
// Every "[AmtPtp] ..." diagnostic message that used to be a raw, always-on
// DbgPrint() call scattered across Device.c/Interrupt.c now goes through
// the AmtTrace() macro below. Output is entirely gated by
// DEVICE_CONTEXT::TraceDebugEnabled, loaded once per device instance
// (AmtPtpDeviceUsbKmCreateDevice, Device.c) from the "DebugMode" REG_DWORD
// under this device's "Device Parameters" registry key - the same
// PLUGPLAY_REGKEY_DEVICE key AmtPalmConfigLoadFromRegistry and friends
// already use (see ConfigIoctl.c).
//
//   DebugMode absent, unreadable, or 0 (the shipped default): AmtTrace()
//   costs one pointer/bool check per call site and nothing is formatted,
//   evaluated, or printed - the variadic arguments themselves (pointers,
//   status codes, etc.) are never even read, not just left unprinted.
//
//   DebugMode = 1: every call fires a DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...)
//   with the same "[AmtPtp] " prefix and message text the old DbgPrint
//   calls used, so existing DebugView/WinDbg filters on "[AmtPtp]" keep
//   working unchanged.
//
// This is a deliberately simple runtime switch, not WPP software tracing -
// there is no WPP preprocessing step wired into this project (the previous
// revision of this file declared a WPP control GUID that nothing ever
// initialized or included), and a plain boolean registry value is
// something a developer can flip per-device instance via regedit (or a
// future AmtPtpConfigGui setting) without extra tooling
// (traceview/tracelog/.pdb-relative .tmf files).
//
// NOT related to the GUI "Live" frame monitor (DEVICE_CONTEXT::LiveEnabled/
// LiveFrame, IOCTL_AMT_PTP_SET_LIVE_ENABLED in ConfigIoctl.c). That is a
// separate, always-available snapshot feed for AmtPtpConfigGui's live
// view and has nothing to do with this debug-trace switch - do not
// conflate the two.

#pragma once

// DEVICE_CONTEXT (Device.h) must already be defined wherever this header
// is included - Driver.h enforces that by including device.h before
// Trace.h. Every .c file in this project includes Driver.h first (see the
// note in ConfigIoctl.h), so this holds everywhere AmtTrace() is used.

EXTERN_C_START

// Registry value name (REG_DWORD, 0 or 1) read from this device's
// "Device Parameters" key. Absent, wrong type, or unreadable => disabled.
#define AMT_TRACE_DEBUGMODE_VALUE_NAME L"DebugMode"

// Component ID passed to DbgPrintEx so trace output can additionally be
// filtered independently of every other driver on the system via the
// standard "Debug Print Filter" registry mechanism
// (HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter),
// on top of this driver's own DebugMode switch.
#define AMT_TRACE_DPFLTR_ID DPFLTR_IHVDRIVER_ID

// Formats and prints one trace line, prefixed with "[AmtPtp] ", via
// DbgPrintEx. Callers should not call this directly - go through AmtTrace()
// below so the DebugMode check happens first. IRQL <= DISPATCH_LEVEL
// (DbgPrintEx itself is documented safe there); this is required because
// several AmtTrace() call sites in Interrupt.c run at DISPATCH_LEVEL
// (the continuous-reader completion routine and EvtUsbTargetPipeReadersFailed).
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtTracePrint(
    _In_ PCSTR Format,
    ...
);

// AmtTrace(Ctx, "fmt", ...) - drop-in replacement for the old
// DbgPrint("[AmtPtp] ...") call sites. Ctx may be NULL (treated as
// disabled; never dereferenced). Written as a do/while(0) macro, not an
// inline function, specifically so the DebugMode check short-circuits
// BEFORE any variadic argument is evaluated when tracing is off - a
// disabled call is just "read one BOOLEAN, branch not taken".
#define AmtTrace(Ctx, Format, ...)                                         \
    do {                                                                   \
        PDEVICE_CONTEXT _amtTraceCtx = (Ctx);                              \
        if (_amtTraceCtx != NULL && _amtTraceCtx->TraceDebugEnabled) {     \
            AmtTracePrint((Format), ##__VA_ARGS__);                        \
        }                                                                  \
    } while (0)

// Loads DEVICE_CONTEXT::TraceDebugEnabled from this device's "DebugMode"
// registry value. Best-effort, same "compiled-in default first, then
// registry override" sequence as AmtPalmConfigLoadFromRegistry/
// AmtPointerConfigLoadFromRegistry/AmtScrollConfigLoadFromRegistry
// (ConfigIoctl.c): any failure (fresh install, no value yet, access
// denied) just leaves tracing at its compiled-in default of FALSE.
_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtTraceLoadDebugModeFromRegistry(
    _In_    WDFDEVICE       Device,
    _Inout_ PDEVICE_CONTEXT DeviceContext
);

// Power-state name helper - moved here from the previous
// DebugUtils.c, which defined these but was never added to the project
// (AmtPtpDeviceUsbKm.vcxproj had no <ClCompile Include="DebugUtils.c" />
// entry, so the file was not even compiled into the driver, and nothing
// declared or called these functions). AmtTrace() call sites in Device.c
// use DbgDevicePowerString to log a readable D0Entry/D0Exit power-state
// name instead of a bare integer.
PCSTR
DbgDevicePowerString(
    _In_ WDF_POWER_DEVICE_STATE Type
);


EXTERN_C_END