// Public.h - Shared declarations for the haptic actuator driver and user apps.
//
// Same rules as AmtPtpDeviceUsbKm's Public.h: only PODs that are safe to
// marshal across the DeviceIoControl boundary belong here. AmtPtpConfigGui
// mirrors these values in HapticsActuator.cs (C#); keep both in sync and bump
// AMT_HAPTIC_INTERFACE_VERSION on any layout change.

#pragma once

// ============================================================================
// Device interface
// ============================================================================

// Exposed by every started AmtHapticDeviceUsbKm device. User mode finds the
// actuator by enumerating this interface class; it never needs a hardware ID
// or a device path baked in.
// {15E33C40-8EC9-4AB3-AFE1-B3E6215F4499}
DEFINE_GUID(GUID_DEVINTERFACE_AMT_HAPTIC,
    0x15e33c40, 0x8ec9, 0x4ab3, 0xaf, 0xe1, 0xb3, 0xe6, 0x21, 0x5f, 0x44, 0x99);

// ============================================================================
// Wire format
// ============================================================================

// Version of every structure below. A client must check AMT_HAPTIC_INFO's
// StructVersion (IOCTL_AMT_HAPTIC_GET_INFO) before it sends a click, so a
// client and a driver built from different revisions refuse to talk instead
// of misreading each other's buffers.
#define AMT_HAPTIC_INTERFACE_VERSION 1

// Click waveforms. The driver owns the actuator payload for each; clients only
// pick one. They are captured actuator waveforms, not a calibrated strength
// scale - how different they feel is device-specific.
#define AMT_HAPTIC_WAVEFORM_LIGHT 0
#define AMT_HAPTIC_WAVEFORM_FIRM  1
#define AMT_HAPTIC_WAVEFORM_COUNT 2

typedef struct _AMT_HAPTIC_CLICK_REQUEST
{
    ULONG StructVersion;   // AMT_HAPTIC_INTERFACE_VERSION
    ULONG Waveform;        // AMT_HAPTIC_WAVEFORM_*
} AMT_HAPTIC_CLICK_REQUEST, *PAMT_HAPTIC_CLICK_REQUEST;

typedef struct _AMT_HAPTIC_INFO
{
    ULONG StructVersion;       // AMT_HAPTIC_INTERFACE_VERSION
    ULONG WaveformCount;       // AMT_HAPTIC_WAVEFORM_COUNT
    ULONG MinClickIntervalMs;  // clicks closer together than this get STATUS_DEVICE_BUSY

    // Lifetime counters of this device instance, for diagnostics.
    ULONG ClicksDelivered;     // transfer completed and fully accepted by the device
    ULONG ClicksThrottled;     // refused by the minimum-interval limit
    ULONG ClicksFailed;        // transfer failed, timed out, or was cancelled

    LONG  LastStatus;          // NTSTATUS of the most recent transfer (0 = none yet)

    ULONG Reserved[5];         // future growth, keep struct size stable
} AMT_HAPTIC_INFO, *PAMT_HAPTIC_INFO;

C_ASSERT(sizeof(AMT_HAPTIC_CLICK_REQUEST) == 8);
C_ASSERT(sizeof(AMT_HAPTIC_INFO) == 48);

// ============================================================================
// IOCTLs
// ============================================================================

#define AMT_HAPTIC_IOCTL_INDEX 0x800

// Output: AMT_HAPTIC_INFO. Needs read access.
#define IOCTL_AMT_HAPTIC_GET_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_HAPTIC_IOCTL_INDEX + 0, METHOD_BUFFERED, FILE_READ_DATA)

// Input: AMT_HAPTIC_CLICK_REQUEST. Fires one click. Completes when the device
// has taken the report. STATUS_DEVICE_BUSY means the click was dropped by the
// minimum-interval limit (flow control, not a fault). Needs write access.
#define IOCTL_AMT_HAPTIC_CLICK \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_HAPTIC_IOCTL_INDEX + 1, METHOD_BUFFERED, FILE_WRITE_DATA)
