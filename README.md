# WellspringPTP 👆

**Make your MacBook trackpad feel at home in Windows.**

WellspringPTP brings Windows Precision Touchpad gestures, Force Touch features, and an easy-to-use Control Center to supported Apple MacBooks running Windows through Boot Camp.

![Wellspring Control Center](https://github.com/user-attachments/assets/4a33bdd7-38ca-47bd-9b4e-68d926ba3eab)

## ✨ What you get

- **Familiar Windows gestures:** smooth scrolling, pinch to zoom, and multi-finger gestures.
- **Force Touch options:** customize the action of a firm press on supported trackpads.
- **Emulated Force Touch:** use a press-and-hold action on supported trackpads without hardware Force Touch.
- **Better typing comfort:** adjustable palm rejection and accidental-touch protection.
- **Personalize your trackpad:** tune pointer, scrolling, and click behavior; save profiles in Control Center.
- **Smarter dragging:** adjustable drag-cancellation distance and Hard Tap hold time for both Force Touch modes.

## 💻 Compatibility

**Tested devices:**

- MacBook Pro 16-inch (2019, MacBookPro16,1)
- MacBook Air 13-inch (2015, MacBookAir7,2)

Other MacBooks with an **internally USB-connected trackpad** may work, but have not all been tested. SPI trackpads and external Magic Trackpad devices are not supported by this driver.

## 📦 Install

1. Download **WellspringPTP-Installer-*.exe** from [GitHub Releases](https://github.com/Osaul4ik/wellspring-ptp/releases).
2. Run the installer as administrator.
3. Choose **GUI + Driver** (recommended) or **Driver only**.
4. Follow the wizard and restart Windows when prompted.
5. Open **Wellspring Control Center** to customize your trackpad.

The installer handles the driver and its test certificate for you. **You don't need to install an INF manually.** It can also enable Windows Test Mode, which is required for the current test-signed driver. Windows may require Secure Boot to be disabled before Test Mode can be enabled; changing Secure Boot settings reduces system protection, so consider this before installing.

**GUI versions:** The *Full* version includes the required .NET runtime. The *Min* version requires .NET 8 Desktop Runtime (x64) separately.

## 🔄 Update or remove

**Updating:** Download and run the newer installer. It normally takes care of replacing the older Wellspring driver. Restart when prompted.

**Uninstalling:** Go to **Windows Settings → Apps → Installed apps**, select **Wellspring Control Center**, and choose **Uninstall**. Follow the wizard to remove the installed components. The combined GUI + Driver installation can also offer to turn off Test Mode and remove saved settings. You may need to restart Windows.

## 🎛️ Make it yours

Open **Wellspring Control Center** to adjust gestures, scrolling, palm rejection, and Force Touch. You can choose an action for Force Touch, set the emulation activation delay, and adjust **Hard Tap hold time** (300–3000 ms; 1300 ms by default).

A press that turns into a drag can start a regular held click. Once an emulated Force Touch action has triggered, that same press won't also trigger a timed Hard Tap.

**Suggested starting point:** Windows pointer speed **4\5\6**. For drag cancellation distance, try a value above **80** and adjust to taste.

## 🎬 See it in action

[MacBook Pro 2019 demo](https://www.youtube.com/shorts/bl8J5mgvfkI) · [MacBook Air 2015 demo](https://www.youtube.com/shorts/R6ZcmY1rli4)

## 🛠️ Troubleshooting

If the trackpad doesn't respond after installing, **restart Windows** first. If you still have a problem, open a [GitHub issue](https://github.com/Osaul4ik/wellspring-ptp/issues) with your MacBook model, Windows version, and a short description of what happened.

For older or unsupported trackpads, see [mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad). For Magic Trackpad 2, see [MagicTrackpad2ForWindows](https://github.com/vitoplantamura/MagicTrackpad2ForWindows).

## ☕ Support

If you enjoy the project, you can [toss a coin on Ko-fi](https://ko-fi.com/osaul4ik). No pressure — feedback and bug reports help too!

## ❤️ Credits & license

Based on the original [mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad) project by imbushuo.

USB driver: **GPL v2** — see [LICENSE-GPL.md](LICENSE-GPL.md). Additional licensing information is in [LICENSE.md](LICENSE.md).
