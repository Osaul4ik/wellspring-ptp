# WellspringPTP

WellspringPTP is a reworked Windows Precision Touchpad driver for Apple MacBook trackpads. The project originated from mac-precision-touchpad and has been redesigned and have a dedicated configuration GUI.

---

# Current Status

Stable 

The driver has been tested on Apple MacBook hardware, including:
* MacBook Pro 16,1 (2019, T2)
* MacBook Air 2015

> **Recommended:**
> Set the Windows pointer speed to **6**.
> Drag cancel distance > 80

---

# Main features

## Windows Precision Touchpad

* Native Windows Precision Touchpad input.
* Native multi-touch gestures (pinch, zoom, scroll, swipe)
* Improved contact matching and lifecycle.

## Palm rejection

Palm rejection is runtime configurable instead of being limited to compile-time constants.

The GUI exposes:
* edge rejection zones;
* major/minor contact thresholds;
* palm shape ratio;
* palm score threshold;
* minimum contact dimensions;
* palm lock policy: by default, once a large palm is detected the touchpad stays disabled until the pad is completely empty; optionally the lock is released as soon as the palm itself has left, so fingers that are still on the pad elsewhere resume.
The configuration is shared through a stable public configuration structure and IOCTL interface.

## Force Touch / Emu Force Touch

For trackpads with a real pressure channel, the driver supports configurable Force Touch click arbitration.
For trackpads without a hardware pressure channel, the driver can emulate Force Touch from a mechanical hard tap.

Available controls include:
* Force Touch / Emu Force Touch enable/disable (master switch for the three options below on pressure-capable trackpads);
  * Force Touch action on/off (hard press sends the Force Tap action);
  * require pressure to activate contact (optional pressure gate);
  * require pressure continuously (optional continuous-pressure requirement);
  The three options are independent: the pressure gates keep working with the Force Touch action switched off.
* pressure threshold;

Force Touch/Emu action:
* context menu;
* middle mouse button;
* double click;
* drag-cancel distance.
---

# Wellspring Control Center

<img width="1438" height="858" alt="Screenshot_2" src="https://github.com/user-attachments/assets/4a33bdd7-38ca-47bd-9b4e-68d926ba3eab" />

The application is a .NET 8 WPF desktop application with Windows Forms support for the tray UI.

The GUI provides:
* Device connection/status information;
* Palm settings;
* Pointer settings;
* Scroll settings;
* Force Touch controls;
* Force Touch emulation controls;
* User profiles;
* Live touch/contact monitoring;
* Touchpad geometry information;
* Driver debug logging control;
* Tray controls for commonly used settings;

Force Touch controls are exposed only when the connected device reports actual Force Touch support. On non-Force-Touch devices, the GUI can expose the software emulation controls instead.

Two GUI versions are included:

- **Min** — requires **.NET 8 Desktop Runtime (x64)** to be installed separately.
- **Full** — includes the required .NET Runtime and does not require a separate .NET installation.

---
# Known Limitations

- Only built-in Apple MacBook touchpads using a USB interface are currently supported. SPI-based touchpads are not currently supported. This refers to the touchpad's internal interface, not an external USB connection.
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

> **Note**
> The driver requires Windows Test Mode;

0. Extract the archive.
1. Run **TestMode.bat**, enter 1, and press Enter to enable Test Mode.
2. **Restart your computer.** (Really)
3. Run **InstallSert.bat** to install the certificate.
4. Right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
5. Reboot
---

# 🚀 Update

### Clean Update

1. Open Device Manager.
2. Under **HID**, find **Wellspring Precision Touchpad**, right-click it, and select **Uninstall device**.
3. Check **Delete the driver software for this device**, then click **Uninstall**.
4. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
5. Reboot
> Use keys (Arrows, Tab, Alt or mouse), when touchpad driver not installed.

### Normal Update

1. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
2. Reboot
---

# Driver Removal

This step is important before installing another Apple touchpad driver (Trackpad++, Magic Utilities, etc.).

1. Open **Device Manager**.
2. Find touchpad driver "Wellspring Precision Touchpad" in "HID" section, open and select "Delete"
3. Enable **Delete the driver software for this device**.
4. Scan for hardware changes or reboot. (Use Alt key in Device Manager and arrows or Win key and choose reboot)

---

# Tested with:

* Driver Verifier
* PREfast
* CodeQL
* Static Analysis

---

### ⚠️ Known Issues

- Very fast Force Touch → Hard Tap: may occasionally be missed before reaching the input queue.
 
---

### YouTube

I quickly recorded two short videos to demonstrate how Wellspring works:

[![MBP 2019 Demo](https://img.youtube.com/vi/bl8J5mgvfkI/0.jpg)](https://www.youtube.com/shorts/bl8J5mgvfkI) 
[![MBA 2015 Demo](https://img.youtube.com/vi/R6ZcmY1rli4/0.jpg)](https://www.youtube.com/shorts/R6ZcmY1rli4)


---

## ☕ Support

If you find this project useful, consider buying me a coffee!

[![Ko-fi](https://img.shields.io/badge/Support%20me%20on-Ko--fi-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/osaul4ik)

---

# Credits

This project is based on the excellent work of the original **mac-precision-touchpad** project by imbushuo.

---

# License
* USB driver — GPL v2