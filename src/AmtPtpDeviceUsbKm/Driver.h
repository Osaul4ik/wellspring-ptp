// Driver definitions. Kernel-mode Driver Framework

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <usbioctl.h> // IOCTL_INTERNAL_USB_CYCLE_PORT - last rung of the
                       // reader-recovery escalation ladder (Interrupt.c)
#include "device.h"
#include "queue.h"

// Trace.h uses PDEVICE_CONTEXT (AmtTrace() macro), so it must come after
// device.h. Every .c file in this project includes Driver.h first (see the
// note at the top of ConfigIoctl.h), so this ordering makes AmtTrace()
// available everywhere without each file needing its own #include "Trace.h".
#include "Trace.h"

// Link to the haptic actuator driver (needs PDEVICE_CONTEXT from device.h).
#include "HapticLink.h"

#include <Hid.h>

EXTERN_C_START

// Shared small geometry/decoding helpers. Every one of these used to be
// re-derived inline at each call site (ActiveContact.c, Match.c, Ptpcore.c,
// Input.c, Palm.c) - centralized here since Driver.h is already the first
// include in every translation unit.

// Absolute difference of two already-widened signed values. Replaces the
// repeated "INT d = a - b; if (d < 0) d = -d;" idiom used by every
// coordinate-delta / threshold check in the contact-matching pipeline.
static __forceinline INT
AmtAbsDelta(_In_ INT A, _In_ INT B)
{
    INT d = A - B;
    return (d < 0) ? -d : d;
}

// Squared Euclidean distance from two signed deltas. Replaces the repeated
// "(LONG)dx * dx + (LONG)dy * dy" used by every nearest-candidate /
// tie-break search (Match.c, Ptpcore.c).
static __forceinline ULONGLONG
AmtDistSq(_In_ INT Dx, _In_ INT Dy)
{
    LONGLONG x = (LONGLONG)Dx;
    LONGLONG y = (LONGLONG)Dy;
    return (ULONGLONG)(x * x + y * y);
}

// Raw HID field -> signed integer. Firmware reports touch_major/minor,
// pressure, and abs_x/abs_y as USHORT-encoded signed values; this was
// previously defined twice, byte-for-byte identically, as
// AmtInputRawToInteger (Input.c) and AmtPalmRawToInteger (Palm.c).
static __forceinline INT
AmtRawToSignedInt(_In_ USHORT x)
{
    return (signed short)x;
}

// Driver-wide (WDFDRIVER) context.
//
// Holds the single, driver-lifetime KMDF control device the GUI talks to
// (see AmtPtpAcquireConfigControlDevice in Device.c for the full
// rationale). This is deliberately NOT part of DEVICE_CONTEXT: the
// control device's named object (\Device\AmtPtpDeviceUsbKm) must survive
// across surprise-removal/re-enumeration of the underlying USB FDO, which
// happens on every sleep/wake where the parent hub momentarily drops the
// device. Tying the control device's lifetime to one FDO instance instead
// of the driver means a GUI handle left open across such a cycle keeps
// the old control device (and its name) alive past the point where the
// next FDO instance tries to create a new one under the same name -
// WdfDeviceInitAssignName then fails with STATUS_OBJECT_NAME_COLLISION,
// which fails that FDO's entire EvtDeviceAdd and leaves the touchpad
// completely non-functional until something else clears the name (e.g.
// the GUI process exiting). Creating the control device once per driver
// load, and only ever re-pointing AMT_CONFIG_CONTROL_CONTEXT::TargetDevice
// at whichever FDO is currently live, removes the collision entirely.
typedef struct _DRIVER_CONTEXT
{
    // The one control device for the lifetime of this driver. NULL until
    // the first successful AmtPtpAcquireConfigControlDevice call, deleted
    // explicitly in AmtPtpDeviceUsbKmEvtDriverContextCleanup - per the WDF
    // control-device-object contract, this is never torn down implicitly
    // by parent/child device-object cleanup the way an FDO's children are.
    WDFDEVICE   ConfigControlDevice;

    // Serializes creation of ConfigControlDevice (first EvtDeviceAdd only)
    // against re-attachment of ConfigControlDevice's TargetDevice pointer
    // (every later EvtDeviceAdd/EvtDeviceContextCleanup) so a fast
    // surprise-removal/re-enumeration cycle can never interleave a
    // detach from the old FDO with an attach from the new one.
    WDFWAITLOCK ConfigControlDeviceLock;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DRIVER_CONTEXT, DriverGetContext)

// WDF driver events.

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtPtpDeviceUsbKmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpDeviceUsbKmEvtDriverContextCleanup;

EXTERN_C_END