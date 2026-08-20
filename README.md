# WellspringPTP

**Windows Precision Touchpad kernel driver for Apple MacBooks**

WellspringPTP is a Windows Precision Touchpad driver focused on delivering a native, responsive, and reliable experience on Apple MacBooks.

The project started as a fork of **mac-precision-touchpad** and has since evolved into a significantly reworked implementation.

---

# Current Status

Stable 

The driver has been extensively tested on **MacBook Pro 16,1 (2019) and Air 2015**

**Haptic feedback is not currently implemented. The Force Touch trackpad's actuator requires a calibration sequence to drive properly, and that process hasn't been reverse-engineered yet — so this feature is blocked until that's figured out.**

> **Recommended:** Set the Windows pointer speed to **8**.

---

# Features

* Windows Precision Touchpad support
* Native multi-touch gestures (pinch, zoom, scroll, swipe)
* Smoother, more accurate finger tracking — fewer dropped or mismatched contacts
* No more cursor jumps or "ghost" touches
* Palm rejection
* Fixed handling of malformed/overflow USB packets
* Force Touch triggers the right-click context menu
* Low-latency input processing

---

# Known Limitations

- Requires Windows Test Mode.
- Haptic feedback is not implemented.
- Only USB Apple touchpads are supported.

---

# Supported Devices

### Fully supported

* Apple MacBook Pro 16,1 (2019, T2)
* MacBook Air 2015 13" (7,2)

### Expected to work (not yet tested)

MacBook Pro:
- MacBook Pro 2015 13" (12,1) — Force Touch, no Touch Bar
- MacBook Pro 2018 15" (15,1)
- MacBook Pro 2018 13", 4× TB3 (15,2)
- MacBook Pro 2019 15" (15,3) — shares the same trackpad/PID as 15,1
- MacBook Pro 2019 13", 2× TB3 (15,4)
- MacBook Pro 2019 16" (16,1)
- MacBook Pro 2020 13", 4× TB3 (16,2)
- MacBook Pro 2020 13", 2× TB3 (16,3)
- MacBook Pro 2020 16" (16,4) — shares the same trackpad/PID as 16,1

MacBook Air:
- MacBook Air 2013 11" (6,1)
- MacBook Air 2013 13" (6,2)
- MacBook Air 2015 11" (7,1) — shares the same trackpad/PID as 6,1
- MacBook Air 2018 (8,1)
- MacBook Air 2019 (8,2) — shares the same trackpad/PID as 8,1 (not verified on real hardware)
- MacBook Air 2020 (9,1)

### Partial

MacBookPro13,x / 14,x (2016-2017, Touch Bar, T1) — PID 0277. Falls back to an imprecise generic entry

### Other MacBooks SPI
MacBookPro13,1 / 14,1 (2016-2017, no Touch Bar) 
MacBook 12" (2015-2017)

- Use https://github.com/imbushuo/mac-precision-touchpad

### Magic TrackPad 2

* Use https://github.com/vitoplantamura/MagicTrackpad2ForWindows

---

# 📦 Installation
0. Extract the archive.
1. Run **TestMode.bat**, enter 1, and press Enter to enable Test Mode.
2. **Restart your computer.** (Really)
3. Run **InstallSert.bat** to install the certificate.
4. Right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.

> **Note**
> The driver currently requires Windows Test Mode because it is not digitally signed.

---


# 🚀 Update

### Clean Update
1. Open Device Manager.
2. Under **HID**, find **Wellspring Precision Touchpad**, right-click it, and select **Uninstall device**.
3. Check **Delete the driver software for this device**, then click **Uninstall**.
4. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
> Use keys (Arrows, Tab, Alt or mouse), when touchpad driver not installed.

### Normal Update
1. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
---

# Driver Removal
This step is important before installing another Apple touchpad driver (Trackpad++, Magic Utilities, etc.).

1. Open **Device Manager**.
2. Find touchpad driver "Wellspring Precision Touchpad" in "HID" section, open and select "Delete"
3. Enable **Delete the driver software for this device**.
4. Scan for hardware changes or reboot. (Use Alt key in Device Manager and arrows or Win key and choose reboot)

---

# Development Status

* ✅ Contact lifecycle redesign
* ✅ Stable contact matching
* ✅ Palm rejection
* ✅ Gesture stability improvements
* ✅ Scroll improvements
* ✅ Cursor jump fixes
* ✅ Force Touch implementation
* ✅ Driver optimization
* ✅ Code audit
* ✅ Micro optimization & tuning

---

## 🧪 BETA RELEASE NOTES

### What's New

- 🔄 **Driver Lifecycle Rework** — redesigned driver lifecycle, initialization, and reinitialization handling.
- 🖥️ **New GUI** — detailed driver configuration and diagnostic tools.
- 🔧 **Pro Mode** — additional advanced configuration options for detailed driver tuning.
- ⚡ **Live Mode** — displays the actual number of active contacts and their current positions in real time.
- 👆 **Contact Preview** — active contacts and their positions are visualized directly in the GUI Preview.
- 🐛 **Debug Mode** — additional diagnostic output for troubleshooting and development.
- ⚙️ **Extended Configuration** — significantly more driver parameters can be configured directly through the GUI.

### ⚠️ Known Issues

- Launching the GUI multiple times can open multiple application instances.
- If **Live Mode** is enabled before entering Sleep, it may become stuck after Resume. Disable and re-enable Live Mode to restore it.
- The GUI context menu may have incorrect scaling when Windows Display Scaling is enabled.
- **Live Mode consumes additional system resources** and should not be enabled unless real-time contact visualization or testing is required.

### 📝 Bug Reports

For useful bug reports:

1. Open **GUI → Settings**.
2. Enable **Debug Mode**.
3. Reproduce the issue.
4. Capture the driver log using **DbgView**.
5. Include the captured log and the configuration used when reporting the issue.

### 📦 GUI Versions

Two GUI versions are included:

- **Min** — requires **.NET 8 Desktop Runtime (x64)** to be installed separately.
- **Full** — includes the required .NET Runtime and does not require a separate .NET installation.
  
---

# Credits

This project is based on the excellent work of the original **mac-precision-touchpad** project by imbushuo.

---

# License
* USB driver — GPL v2
