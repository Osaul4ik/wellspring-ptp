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

- `src/AmtPtpDeviceUsbKm` (the touchpad driver): decides when a hard press is
  a Force Tap and plays the haptic click itself (`HapticLink.c`), by sending
  one IOCTL to the actuator driver's device interface. It needs no user-mode
  program, so it works when Control Center is closed.
- `src/AmtHapticDeviceUsbKm`: the actuator driver, a small KMDF function driver
  for one interface (`MI_03`) of the trackpad's composite USB device. It is
  independent of the touchpad driver: the touchpad driver filters `MI_02`, this
  one owns `MI_03`. Its only job is to play a fixed click waveform on request.
- Control Center (`src/AmtPtpConfigGui`): the settings, the live pressure
  meter, the status line and the Test click (`HapticsEngine.cs`). It only
  observes and assists; it plays nothing on a hard press.

### The link between the two drivers (`HapticLink.c`)

- **One contract.** `HapticLink.c` includes the actuator driver's own
  `Public.h`; there is no hand-made mirror of the GUID/IOCTL/structure on the
  kernel side. (`HapticsActuator.cs` still mirrors it for user mode.)
- **States** (`AMT_HAPTIC_LINK_STATE_*` in the touchpad driver's `Public.h`):
  `ABSENT` (no actuator interface), `READY` (open, clicks are sent), `CLOSED`
  (the actuator is being removed, or a re-open after a cancelled removal
  failed; the next interface arrival replaces the target) and `INCOMPATIBLE`
  (the actuator driver answered `STATUS_REVISION_MISMATCH`).
- **A haptic click that cannot be played is treated as switched off.** The frame
  path takes one snapshot per frame (`ForceTouchHapticActive` = haptic click on
  AND link `READY`). Only then does the haptic click keep the hard-press
  arbitration running. Without this, "haptic click on, Force Touch action off"
  on a model the actuator driver does not bind to would hold every button-down
  back and swallow hard presses completely - for a click that never comes.
- **No allocation on the click path.** The request is created and formatted once
  when the link opens (PASSIVE_LEVEL); a click only re-formats it.
- **Diagnostics.** `IOCTL_AMT_PTP_GET_HAPTIC_LINK_INFO` returns the state and
  counters (requested / delivered / throttled / failed / dropped, last status).
  Control Center uses it for the status line; with a touchpad driver that
  predates the IOCTL it falls back to probing the actuator interface.

## Settings

Haptic settings are ordinary driver configuration: three fields of the pointer
config (`ForceTapThreshold`, `ForceTouchHapticEnabled`,
`ForceTouchHapticWaveform`). They therefore follow profile switching, **Reset to
defaults**, profile export/import and backup/restore together with the rest of
the Pointer settings, and are applied by the bottom-bar **Save**.

There is no separate "Haptics" object in profiles, profile files (format 4) or
backups any more - the pointer config is the single source of truth. Files
written while Control Center played the click itself still load: the old
object's on/off switch and waveform are folded into the pointer config by
`PointerConfig.Migrate`, which is also the one place that knows the pointer
config's version history (profile store, profile files and backups all go
through it). The old object's separate haptic threshold is not migrated; the
profile's own `ForceTapThreshold` stays.

The status line under the settings reflects the driver's **applied**
configuration and its link to the actuator driver, not the controls: an edit
counts once it has been saved.
The Haptics settings now live in the same **Force Touch & Haptics** group as
the Force Touch options. The group is shown only on trackpads with a pressure
sensor.

Defaults: Force Touch on, Force Touch action on with the context menu, haptic
click on with the light waveform, Pressure needed 270.

**Pressure needed** is the single pressure threshold for the whole group: the
Force Touch action (driver `ForceTapThreshold`) and the second haptic click both
fire at exactly this value. There is no separate Force Touch threshold any more.
The slider moves in 1-unit steps (arrow keys; PageUp/PageDown for 10) over
50-1000, and it stays active even if the second haptic click is turned off,
because it also drives Force Touch.

## Behavior

The physical click arbitrates a press. While the button is held and the finger
has not moved past **Drag cancel distance**, pressing harder than **Pressure
needed** makes the driver fire the haptic click and the Force Tap action
(for example the context menu) at the same moment. There is no hold time: the
press can rest as long as you like before pressing harder. It does not repeat
while held; release the button to rearm.

If the finger moves past the drag-cancel distance first, the press is an
ordinary click or drag: no haptic click and no Force Tap. A Force Tap that has
already fired stays committed.

Drag cancel is always on; only its distance is adjustable.

Trade-off: while a press is undecided the ordinary button-down is held back, so
a click-and-hold only starts (as a drag) once the finger moves past the
drag-cancel distance, and a plain click is reported on release.

Settings (including **Pressure needed**, the haptic switch and the waveform)
are stored by the driver in the registry, so they apply at every boot without
Control Center. The Control Center only edits them and, while the toolbar's
**Live** switch is on, shows the live pressure to help choose a threshold. The
raw scale is device-specific. Light/firm are captured waveform names, not a
guarantee of perceptibly different strength. **Test click** plays the selected
waveform without pressing.

To get only the haptic feedback, turn off **Force Touch action**.

## Installing and removing the actuator driver

The installer (`WellspringPTP-Setup-*.exe`, built by `BuildAll.yml`) installs
the haptic actuator driver together with the touchpad driver (same Test Mode /
certificate / reboot flow; the haptic package is signed with the same
certificate) and removes both on uninstall. Older haptic packages are removed
before a reinstall, so oemNN.inf copies do not pile up. If the haptic driver
fails to install, the touchpad driver is still installed and only the haptic
click is missing.

`DriverInstallPrompt.bat` (shipped in the driver archive) does the same from a
command line: run it for an Install / Uninstall menu, or pass the mode
directly: `DriverInstallPrompt.bat AmtPtpDeviceUsbKm install` /
`... uninstall`. It looks for `AmtHapticDeviceUsbKm.*` next to itself or in a
sibling `HapticDriver` folder. `InstallCert.bat` only imports the certificate
and is unchanged.

## Actuator driver

The driver is organised like the touchpad driver (`Driver`, `Device`, `Queue`,
`Trace`, `Public`) plus `Actuator.c`, which holds everything hardware-specific.

- **Interface contract** (`Public.h`, included directly by the touchpad
  driver's `HapticLink.c` and mirrored in `HapticsActuator.cs`): a device
  interface GUID and two IOCTLs, `GET_INFO` and `CLICK`, with versioned
  structures. Control Center checks the driver's interface version when it opens
  the device and refuses a driver built from a different revision; the touchpad
  driver stops sending after the first `STATUS_REVISION_MISMATCH`.
- **Nobody parks a handle on it.** Control Center opens the interface, sends one
  or two IOCTLs and closes it again (status probe, Test click). An open
  user-mode handle would make a driver update fail and, after the trackpad drops
  off the bus across sleep, would keep the old device instance from being
  deleted - and the same instance from coming back. The touchpad driver's link
  does keep it open, but handles query-remove / remove through its I/O target.
- **Identification**: it sends nothing to a device it has not positively
  identified (supported vendor/product, exactly one interface, interface number
  3, HID class). Supported products are one table in `Actuator.c`; the INF lists
  the same IDs. Only `05AC:0340` (MacBookPro16,1) is enabled - add a model only
  after confirming it exposes the actuator on `MI_03` and accepts these reports.
- **Click path**: the click is an asynchronous HID SET_REPORT on a request and
  buffer created once in `EvtDevicePrepareHardware` (`AmtHapticPrepareTransfer`)
  for the USB target and formatted once there, so the IRP, the framework's USB
  context and the URB all exist before the first click: a click allocates
  nothing and cannot fail for lack of memory. Clicks closer than 100 ms are
  refused with `STATUS_DEVICE_BUSY` (flow control: the actuator is not a toy to
  hammer); a click that failed before reaching the device does not start that
  cool-down.
- **Execution level**: the default (callbacks at IRQL <= DISPATCH_LEVEL). The
  touchpad driver sends its click from its USB completion path; with a
  passive-level device every such click would first be handed to a system worker
  thread, adding scheduling latency to the haptic response.
- **Power and removal**: no idle power management (the first click after an idle
  suspend would pay resume latency). `EvtIoStop` cancels an in-flight transfer on
  removal and lets it finish across a suspend.
- **Access**: set by the INF (`Security` under the device's hardware key; an FDO
  must not call `WdfDeviceInitAssignSDDLString`, which fails the start with
  Code 31) and applied to the device interface by `FILE_DEVICE_SECURE_OPEN`:
  administrators and SYSTEM full access, other signed-in users read/write. That
  lets an unelevated Control Center fire a click, and no more than that.
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
`HapticDriver` folder of the release archives and also embeds it in the
installer (see above), and
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

Sleep/wake recovery and long-term stability have not been established. The
touchpad driver re-finds the actuator driver by itself when its device
interface re-appears (for example after a replug or resume).

## Attribution

WellspringPTP is by Osaul4ik and derives from imbushuo/mac-precision-touchpad.
The captured actuator waveforms and protocol are adapted from André Eikmeyer's
`kaiT2en/KaiT2en-Fedora`, specifically
`modules/t2_trackpad_actuator/t2_trackpad_actuator.c` (GPL-2.0-only):
https://github.com/kaiT2en/KaiT2en-Fedora

The new actuator/helper source is GPL-2.0-only; see `LICENSE-GPL.md`.