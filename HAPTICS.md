# Pressure haptics extension

This extension adds a **Haptics** group to the **Pointer** tab of Wellspring
Control Center (next to Force Touch). It connects real pressure measurements
from Wellspring to a separate Windows KMDF actuator driver, so a firm press
after the physical click fires a second haptic click.

**Status:** the actuator protocol was exercised on a 2019 16-inch MacBook Pro
(`MacBookPro16,1`, USB `05AC:0340`) by the prototype driver this one replaces.
`AmtHapticDeviceUsbKm` itself has been compile-checked but **not yet run on
hardware**. Other models are not enabled by the INF.

## Components

- Control Center (`src/AmtPtpConfigGui`): the settings, and the engine that
  does the work (`HapticsEngine.cs`). No separate program is involved.
- `src/AmtHapticDeviceUsbKm`: the actuator driver, a small KMDF function driver
  for one interface (`MI_03`) of the trackpad's composite USB device. It is
  independent of the touchpad driver: the touchpad driver filters `MI_02`, this
  one owns `MI_03`, and neither knows about the other. Its only job is to play a
  fixed click waveform on request.

## Settings

Haptic settings are part of the **profile**, like Palm, Pointer and Scroll:
they follow profile switching, **Reset to defaults**, profile export/import and
backup/restore, and they are applied by the bottom-bar **Save**. A profile or
file written before haptics existed keeps the current haptic settings on load.
The group is shown only on trackpads with a pressure sensor, like Force Touch.

## Behavior

A background thread inside Control Center reads Wellspring's live pressure
frames and fires one click through the actuator driver. It keeps working while
the window is hidden in the tray, but not after the app is closed (use
"Start with Windows" to have it available after sign-in). While haptics are off,
or the actuator driver is not installed, it asks the driver for nothing.

The default threshold is 265 raw pressure units, with release at 70% of that
value. Two fresh samples above threshold trigger one waveform. Holding the
press does not repeat it; pressure must fall below the release threshold to
rearm. It also requires one confident non-palm contact and physical
button-down, imposes a 250 ms cooldown, and resets after a stale feed.

The raw scale is device-specific; use the live meter to pick a threshold.
Light/firm are captured waveform names, not a guarantee of perceptibly
different strength on every device. **Test click** plays the selected waveform
without pressing, saved or not.

Force Touch's mouse action should be disabled ("Force Touch action" in the
Pointer tab) when only haptic feedback is wanted. This extension does not
change pointer settings automatically.

The Live view and haptics share the driver's live stream (`DeviceIo.SetLiveConsumer`):
it stays on while either wants it, so hiding the window to the tray or turning
Live off does not stop haptics. The engine recovers on its own after the driver
restarts, sleep/wake, or the actuator reappearing.

## Actuator driver

The driver is organised like the touchpad driver (`Driver`, `Device`, `Queue`,
`Trace`, `Public`) plus `Actuator.c`, which holds everything hardware-specific.

- **Interface contract** (`Public.h`, mirrored in `HapticsActuator.cs`): a device
  interface GUID and two IOCTLs, `GET_INFO` and `CLICK`, with versioned
  structures. Control Center checks the driver's interface version when it opens
  the device and refuses a driver built from a different revision.
- **Identification**: it sends nothing to a device it has not positively
  identified (supported vendor/product, exactly one interface, interface number
  3, HID class). Supported products are one table in `Actuator.c`; the INF lists
  the same IDs. Only `05AC:0340` (MacBookPro16,1) is enabled - add a model only
  after confirming it exposes the actuator on `MI_03` and accepts these reports.
- **Click path**: the click is an asynchronous HID SET_REPORT on a request and
  buffer allocated once, so a click allocates nothing and cannot fail for lack of
  memory. Clicks closer than 100 ms are refused with `STATUS_DEVICE_BUSY`
  (flow control: the actuator is not a toy to hammer).
- **Power and removal**: no idle power management (the first click after an idle
  suspend would pay resume latency). `EvtIoStop` cancels an in-flight transfer on
  removal and lets it finish across a suspend.
- **Access**: opens are checked against the SDDL set in `Device.c`: administrators
  and SYSTEM full access, other signed-in users read/write. That lets an
  unelevated Control Center fire a click, and no more than that.
- **Diagnostics**: `GET_INFO` reports delivered/throttled/failed click counters
  and the last transfer status. Set `DebugMode` (REG_DWORD, 1) under the device's
  `Device Parameters` key to get `[AmtHaptic] ...` lines in DebugView.

The waveform payloads are the actuator's own protocol; they follow the t2linux
project's `t2_trackpad_actuator` captures (GPL-2.0, like this project). Do not
edit a byte without testing it on the hardware.

## Build and validation

Control Center builds exactly as before; there is nothing extra to publish or
install for the GUI side. The actuator driver builds with the same toolchain as
the touchpad driver (`AmtHapticDeviceUsbKm.vcxproj`, same configuration matrix).
Like the touchpad driver, its INF is not part of the project: CI stamps it
(StampInf), catalogs it (Inf2Cat) and signs it. `BuildAll` ships it in a separate
`HapticDriver` folder of the release archives (not in the installer), and
`StaticAnalysis` runs the same strict PREfast pass over it. Driver signing and
installation are otherwise deliberately separate. Do not distribute a local
private signing key, certificate trust setup, or machine-specific driver backup
with source.

## Experimental driver deployment

Current locally built drivers require appropriate signing/Test Mode. Installing
or testing kernel drivers can crash Windows or disrupt input. Keep a keyboard
or external mouse available and export the existing pointer package first.
Do not disable Test Mode while dependent experimental drivers are installed.
Revert each affected interface to its original driver before cleaning up test
trust. Never replace the composite parent, keyboard, or Apple USB host driver.

Sleep/wake recovery and long-term stability have not been established. If the
pressure feed disconnects, the helper stops; the panel then offers Start helper.

## Attribution

WellspringPTP is by Osaul4ik and derives from imbushuo/mac-precision-touchpad.
The captured actuator waveforms and protocol are adapted from André Eikmeyer's
`kaiT2en/KaiT2en-Fedora`, specifically
`modules/t2_trackpad_actuator/t2_trackpad_actuator.c` (GPL-2.0-only):
https://github.com/kaiT2en/KaiT2en-Fedora

The new actuator/helper source is GPL-2.0-only; see `LICENSE-GPL.md`.
