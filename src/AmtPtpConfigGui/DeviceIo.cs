using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Talks to AmtPtpDeviceUsbKm.sys through its KMDF control-device
    /// symbolic link (\\.\AmtPtpDeviceUsbKm) using DeviceIoControl.
    /// The USB/HID filter itself is intentionally not opened by the GUI.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct DeviceInfo
    {
        public uint StructVersion;
        public ushort VendorId;
        public ushort ProductId;
        public byte SupportsForceTouch;
        public byte Reserved0;
        public byte Reserved1;
        public byte Reserved2;
    }

    /// <summary>
    /// AMT_HAPTIC_LINK_INFO (Public.h): the touchpad driver's link to the
    /// haptic actuator driver and what happened to the clicks it asked for.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct HapticLinkInfo
    {
        public uint StructVersion;
        public uint State;
        public uint ClicksRequested;
        public uint ClicksDelivered;
        public uint ClicksThrottled;
        public uint ClicksFailed;
        public uint ClicksDropped;
        public int LastStatus;
        public uint Reserved0;
        public uint Reserved1;
        public uint Reserved2;
        public uint Reserved3;

        // Must match AMT_HAPTIC_LINK_INFO_VERSION / AMT_HAPTIC_LINK_STATE_*.
        public const uint CurrentVersion = 1;
        public const uint StateAbsent = 0;
        public const uint StateReady = 1;
        public const uint StateClosed = 2;
        public const uint StateIncompatible = 3;
    }

    public sealed class DeviceIo : IDisposable
    {
        // AMT_PTP_IOCTL_INDEX (0x900) + offset, mirrored from Public.h.
        private const uint FileDeviceUnknown = 0x00000022;
        private const uint MethodBuffered = 0;
        private const uint FileReadAccess = 1;
        private const uint FileWriteAccess = 2;
        private const uint AmtPtpIoctlIndex = 0x900;

        private static uint CtlCode(
            uint deviceType,
            uint function,
            uint method,
            uint access) =>
            (deviceType << 16) |
            (access << 14) |
            (function << 2) |
            method;

        public static readonly uint IoctlGetDeviceInfo =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 12, MethodBuffered, FileReadAccess);

        public static readonly uint IoctlGetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 0,
                MethodBuffered,
                FileReadAccess);

        public static readonly uint IoctlSetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 1,
                MethodBuffered,
                FileWriteAccess);

        public static readonly uint IoctlGetPadGeometry =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 2,
                MethodBuffered,
                FileReadAccess);

        public static readonly uint IoctlResetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 3,
                MethodBuffered,
                FileWriteAccess);

        public static readonly uint IoctlSetLiveEnabled =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 4,
                MethodBuffered,
                FileWriteAccess);

        public static readonly uint IoctlGetLiveFrame =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 5,
                MethodBuffered,
                FileReadAccess);

        public static readonly uint IoctlGetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 6,
                MethodBuffered,
                FileReadAccess);

        public static readonly uint IoctlSetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 7,
                MethodBuffered,
                FileWriteAccess);

        public static readonly uint IoctlResetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 8,
                MethodBuffered,
                FileWriteAccess);

        public static readonly uint IoctlGetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 9, MethodBuffered, FileReadAccess);
        public static readonly uint IoctlSetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 10, MethodBuffered, FileWriteAccess);
        public static readonly uint IoctlResetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 11, MethodBuffered, FileWriteAccess);

        // Runtime debug-trace switch (DEVICE_CONTEXT::TraceDebugEnabled -
        // driver-side Trace.h/Trace.c). Plain ULONG (0/1) in and out, mirrors
        // Public.h's IOCTL_AMT_PTP_GET/SET_DEBUG_MODE exactly.
        public static readonly uint IoctlGetDebugMode =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 13, MethodBuffered, FileReadAccess);
        public static readonly uint IoctlSetDebugMode =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 14, MethodBuffered, FileWriteAccess);

        // State + click counters of the touchpad driver's link to the haptic
        // actuator driver (IOCTL_AMT_PTP_GET_HAPTIC_LINK_INFO).
        public static readonly uint IoctlGetHapticLinkInfo =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 15, MethodBuffered, FileReadAccess);

        private const string ControlDevicePath = @"\\.\AmtPtpDeviceUsbKm";

        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x1;
        private const uint FileShareWrite = 0x2;
        private const uint OpenExisting = 3;

        private readonly object _ioLock = new();
        private SafeFileHandle? _handle;
        private IntPtr _liveFrameBuffer;
        private int _liveFrameBufferSize;

        public bool IsConnected
        {
            get
            {
                lock (_ioLock)
                {
                    return _handle != null && !_handle.IsInvalid;
                }
            }
        }

        /// <summary>
        /// Human-readable reason the last TryConnect() call failed.
        /// </summary>
        public string LastErrorMessage { get; private set; } =
            string.Empty;

        private bool Fail(string step)
        {
            int err = Marshal.GetLastWin32Error();

            string text =
                new Win32Exception(err).Message.Trim();

            LastErrorMessage =
                $"{step}: {text} (Win32 {err})";

            return false;
        }

        /// <summary>
        /// Opens the driver's dedicated KMDF control device.
        /// </summary>
        public bool TryConnect()
        {
            lock (_ioLock)
            {
                DisconnectUnsafe();

                LastErrorMessage = string.Empty;

                var handle = CreateFile(
                    ControlDevicePath,
                    GenericRead | GenericWrite,
                    FileShareRead | FileShareWrite,
                    IntPtr.Zero,
                    OpenExisting,
                    0,
                    IntPtr.Zero);

                if (handle.IsInvalid)
                {
                    return Fail($"CreateFile('{ControlDevicePath}')");
                }

                int liveFrameSize = Marshal.SizeOf<LiveFrame>();
                IntPtr liveFrameBuffer;
                try
                {
                    liveFrameBuffer = Marshal.AllocHGlobal(liveFrameSize);
                }
                catch
                {
                    handle.Dispose();
                    throw;
                }

                _handle = handle;
                _liveOnDriver = false;
                _liveFrameBufferSize = liveFrameSize;
                _liveFrameBuffer = liveFrameBuffer;
                return true;
            }
        }

        public void Disconnect()
        {
            lock (_ioLock)
            {
                DisconnectUnsafe();
            }
        }

        private void DisconnectUnsafe()
        {
            _handle?.Dispose();
            _handle = null;
            _liveOnDriver = false;

            if (_liveFrameBuffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_liveFrameBuffer);
                _liveFrameBuffer = IntPtr.Zero;
            }

            _liveFrameBufferSize = 0;
        }

        public bool TryGetDeviceInfo(out DeviceInfo info)
        {
            lock (_ioLock)
            {
                info = default;
                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<DeviceInfo>();
                IntPtr outBuf = Marshal.AllocHGlobal(size);
                try
                {
                    bool ok = DeviceIoControl(
                        _handle,
                        IoctlGetDeviceInfo,
                        IntPtr.Zero,
                        0,
                        outBuf,
                        (uint)size,
                        out uint bytesReturned,
                        IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    info = Marshal.PtrToStructure<DeviceInfo>(outBuf);
                    return info.StructVersion == 1;
                }
                finally
                {
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        /// <summary>
        /// Asks the touchpad driver whether it can actually play the second
        /// haptic click (its link to the actuator driver) and how the clicks
        /// it requested ended. False when not connected, and also when the
        /// installed driver predates this IOCTL - callers then fall back to
        /// looking for the actuator driver's interface themselves.
        /// </summary>
        public bool TryGetHapticLinkInfo(out HapticLinkInfo info)
        {
            lock (_ioLock)
            {
                info = default;
                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<HapticLinkInfo>();
                IntPtr outBuf = Marshal.AllocHGlobal(size);
                try
                {
                    bool ok = DeviceIoControl(
                        _handle,
                        IoctlGetHapticLinkInfo,
                        IntPtr.Zero,
                        0,
                        outBuf,
                        (uint)size,
                        out uint bytesReturned,
                        IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    info = Marshal.PtrToStructure<HapticLinkInfo>(outBuf);
                    return info.StructVersion == HapticLinkInfo.CurrentVersion;
                }
                finally
                {
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        public string GetDeviceModelDisplay() => GetDeviceModelDisplay(out _);

        /// <summary>
        /// Same model string as before, plus whether the connected trackpad
        /// supports Force Touch click arbitration. Used by the GUI to hide
        /// the Force Touch settings group and tray menu entries entirely on
        /// hardware that doesn't have Force Touch, instead of showing
        /// controls for a feature the driver will just ignore.
        ///
        /// True/false come from the driver's own GET_DEVICE_INFO answer
        /// when available (authoritative - the driver already resolved this
        /// per-model), or from a small SMBIOS product-name table for older
        /// driver builds that predate that IOCTL. Null means "couldn't
        /// determine" (no device, or an unrecognized SMBIOS product name);
        /// callers should treat null as "assume supported" so the setting
        /// is never hidden on inconclusive data.
        /// </summary>
        public string GetDeviceModelDisplay(out bool? supportsForceTouch)
        {
            if (TryGetDeviceInfo(out var info) && info.ProductId != 0)
            {
                string model = info.ProductId switch
                {
                    0x0272 or 0x0273 or 0x0274 => "MacBookPro12,1 · 2015 13-inch",
                    0x0290 or 0x0291 => "MacBookAir6/7,x · pre-Force Touch",
                    0x0277 => "MacBookPro13,2 · 2016 15-inch",
                    0x027A => "MacBookAir8,1 · 2018",
                    0x027B => "MacBookPro15,2 · 2018 13-inch",
                    0x027C => "MacBookPro15,1 · 2018 15-inch",
                    0x027D => "MacBookPro15,4 · 2019 13-inch",
                    0x027E => "MacBookPro16,2 · 2020 13-inch",
                    0x027F => "MacBookPro16,3 · 2020 13-inch",
                    0x0280 => "MacBookAir9,1 · 2020",
                    0x0340 => "MacBookPro16,1 · 2019 16-inch",
                    _ => $"Apple Internal Trackpad · PID 0x{info.ProductId:X4}"
                };

                supportsForceTouch = info.SupportsForceTouch != 0;
                return supportsForceTouch == true
                    ? $"{model} · Force Touch"
                    : model;
            }

            // The driver-side GET_DEVICE_INFO IOCTL was added after the original
            // GUI. Fall back to the Windows SMBIOS product name so the model is
            // still shown when an older driver build is installed.
            string? hostProduct = null;
            try
            {
                using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                    @"HARDWARE\DESCRIPTION\System\BIOS");
                hostProduct = key?.GetValue("SystemProductName") as string;
            }
            catch
            {
                // Keep a safe generic fallback.
            }

            switch (hostProduct)
            {
                case "MacBookPro16,1":
                case "MacBookPro16,2":
                case "MacBookPro16,3":
                case "MacBookPro15,1":
                case "MacBookPro15,2":
                case "MacBookPro15,4":
                    supportsForceTouch = true;
                    return $"{hostProduct} · {DescribeSmbiosModel(hostProduct)} · Force Touch";
                // Known pre-Force-Touch hardware: report false explicitly
                // instead of falling into the "unknown -> assume supported"
                // default below. Without this, a MacBook Air (or an older
                // pre-2015 Retina Pro) whose installed driver predates the
                // GET_DEVICE_INFO IOCTL would fall through to "unknown" and
                // the Force Touch group/tray items would incorrectly show,
                // even though the SMBIOS name already tells us the hardware
                // has no Force Touch trackpad.
                case "MacBookAir6,1":
                case "MacBookAir6,2":
                case "MacBookAir7,1":
                case "MacBookAir7,2":
                    supportsForceTouch = false;
                    return $"{hostProduct} · pre-Force Touch";
                default:
                    supportsForceTouch = null;
                    return !string.IsNullOrWhiteSpace(hostProduct)
                        ? hostProduct
                        : "Apple Precision Touchpad · model unavailable";
            }
        }

        private static string DescribeSmbiosModel(string hostProduct) => hostProduct switch
        {
            "MacBookPro16,1" => "2019 16-inch",
            "MacBookPro16,2" => "2020 13-inch",
            "MacBookPro16,3" => "2020 13-inch",
            "MacBookPro15,1" => "2018 15-inch",
            "MacBookPro15,2" => "2018 13-inch",
            "MacBookPro15,4" => "2019 13-inch",
            _ => hostProduct
        };

        private bool TryControlIoctl(uint code)
        {
            lock (_ioLock)
            {
                if (_handle == null || _handle.IsInvalid)
                    return false;

                return DeviceIoControl(
                    _handle,
                    code,
                    IntPtr.Zero,
                    0,
                    IntPtr.Zero,
                    0,
                    out _,
                    IntPtr.Zero);
            }
        }

        public bool TryGetPalmConfig(
            out PalmConfig config)
        {
            config = PalmConfig.Default;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlGetPalmConfig,
                null,
                ref config);
        }

        public bool TrySetPalmConfig(
            PalmConfig request,
            out PalmConfig applied)
        {
            applied = request;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlSetPalmConfig,
                (PalmConfig?)request,
                ref applied);
        }

        public bool TryResetPalmConfig(
            out PalmConfig applied)
        {
            applied = PalmConfig.Default;

            if (!TryControlIoctl(IoctlResetPalmConfig))
                return false;

            return TryGetPalmConfig(out applied);
        }

        public bool TryGetPointerConfig(
            out PointerConfig config)
        {
            config = PointerConfig.Default;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlGetPointerConfig,
                null,
                ref config);
        }

        public bool TrySetPointerConfig(
            PointerConfig request,
            out PointerConfig applied)
        {
            applied = request;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlSetPointerConfig,
                (PointerConfig?)request,
                ref applied);
        }

        public bool TryResetPointerConfig(
            out PointerConfig applied)
        {
            applied = PointerConfig.Default;

            if (!TryControlIoctl(IoctlResetPointerConfig))
                return false;

            return TryGetPointerConfig(out applied);
        }

        public bool TryGetScrollConfig(out ScrollConfig config)
        {
            config = ScrollConfig.Default;
            if (!IsConnected) return false;
            return TryIoctl(IoctlGetScrollConfig, null, ref config);
        }

        public bool TrySetScrollConfig(ScrollConfig request, out ScrollConfig applied)
        {
            applied = request;
            if (!IsConnected) return false;
            return TryIoctl(IoctlSetScrollConfig, (ScrollConfig?)request, ref applied);
        }

        public bool TryResetScrollConfig(out ScrollConfig applied)
        {
            applied = ScrollConfig.Default;
            if (!TryControlIoctl(IoctlResetScrollConfig))
                return false;
            return TryGetScrollConfig(out applied);
        }

        public bool TryGetPadGeometry(
            out PadGeometry geometry)
        {
            lock (_ioLock)
            {
                geometry = PadGeometry.Fallback;

                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<PadGeometry>();
                IntPtr outBuf = Marshal.AllocHGlobal(size);
                try
                {
                    bool ok = DeviceIoControl(
                        _handle,
                        IoctlGetPadGeometry,
                        IntPtr.Zero,
                        0,
                        outBuf,
                        (uint)size,
                        out uint bytesReturned,
                        IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    var result = Marshal.PtrToStructure<PadGeometry>(outBuf);
                    if (!result.IsValid)
                        return false;

                    geometry = result;
                    return true;
                }
                finally
                {
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        private bool TryIoctl(
            uint code,
            PalmConfig? input,
            ref PalmConfig output)
        {
            lock (_ioLock)
            {
                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<PalmConfig>();
                IntPtr inBuf = IntPtr.Zero;
                IntPtr outBuf = Marshal.AllocHGlobal(size);

                try
                {
                    uint inSize = 0;
                    if (input.HasValue)
                    {
                        inBuf = Marshal.AllocHGlobal(size);
                        Marshal.StructureToPtr(input.Value, inBuf, false);
                        inSize = (uint)size;
                    }

                    bool ok = DeviceIoControl(
                        _handle, code, inBuf, inSize, outBuf, (uint)size,
                        out uint bytesReturned, IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    output = Marshal.PtrToStructure<PalmConfig>(outBuf);
                    return true;
                }
                finally
                {
                    if (inBuf != IntPtr.Zero)
                        Marshal.FreeHGlobal(inBuf);
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        private bool TryIoctl(
            uint code,
            PointerConfig? input,
            ref PointerConfig output)
        {
            lock (_ioLock)
            {
                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<PointerConfig>();
                IntPtr inBuf = IntPtr.Zero;
                IntPtr outBuf = Marshal.AllocHGlobal(size);

                try
                {
                    uint inSize = 0;
                    if (input.HasValue)
                    {
                        inBuf = Marshal.AllocHGlobal(size);
                        Marshal.StructureToPtr(input.Value, inBuf, false);
                        inSize = (uint)size;
                    }

                    bool ok = DeviceIoControl(
                        _handle, code, inBuf, inSize, outBuf, (uint)size,
                        out uint bytesReturned, IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    output = Marshal.PtrToStructure<PointerConfig>(outBuf);
                    return true;
                }
                finally
                {
                    if (inBuf != IntPtr.Zero)
                        Marshal.FreeHGlobal(inBuf);
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        private bool TryIoctl(
            uint code,
            ScrollConfig? input,
            ref ScrollConfig output)
        {
            lock (_ioLock)
            {
                if (_handle == null || _handle.IsInvalid)
                    return false;

                int size = Marshal.SizeOf<ScrollConfig>();
                IntPtr inBuf = IntPtr.Zero;
                IntPtr outBuf = Marshal.AllocHGlobal(size);

                try
                {
                    uint inSize = 0;
                    if (input.HasValue)
                    {
                        inBuf = Marshal.AllocHGlobal(size);
                        Marshal.StructureToPtr(input.Value, inBuf, false);
                        inSize = (uint)size;
                    }

                    bool ok = DeviceIoControl(
                        _handle, code, inBuf, inSize, outBuf, (uint)size,
                        out uint bytesReturned, IntPtr.Zero);

                    if (!ok || bytesReturned < (uint)size)
                        return false;

                    output = Marshal.PtrToStructure<ScrollConfig>(outBuf);
                    return true;
                }
                finally
                {
                    if (inBuf != IntPtr.Zero)
                        Marshal.FreeHGlobal(inBuf);
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        /// <summary>
        /// Who wants the driver's live frame stream. The driver gives the
        /// stream to one owner per handle, so the Live view and the haptics
        /// engine share it through a consumer mask instead of toggling it
        /// independently (which would let either one switch it off under
        /// the other).
        /// </summary>
        public enum LiveConsumer
        {
            View = 1,
            Haptics = 2,
        }

        // Bitmask of LiveConsumer values that currently want the stream, and
        // what the driver was last told on the current handle (null = not
        // known, e.g. after a failed frame read, so the next change is always
        // sent). Both guarded by _ioLock.
        private int _liveConsumers;
        private bool? _liveOnDriver = false;

        /// <summary>The Live view's switch (see SetLiveConsumer).</summary>
        public bool SetLiveEnabled(bool enabled) =>
            SetLiveConsumer(LiveConsumer.View, enabled);

        /// <summary>
        /// Registers or drops one consumer. The driver is only told when the
        /// overall wish changes: first consumer on, last consumer off.
        /// Re-asserting an unchanged wish is a cheap no-op, except when the
        /// driver's state is unknown (a frame read failed), when it is re-sent.
        /// </summary>
        public bool SetLiveConsumer(LiveConsumer consumer, bool enabled)
        {
            lock (_ioLock)
            {
                int mask = (int)consumer;
                _liveConsumers = enabled ? (_liveConsumers | mask) : (_liveConsumers & ~mask);

                if (_handle == null || _handle.IsInvalid)
                {
                    _liveOnDriver = false;
                    return false;
                }

                bool want = _liveConsumers != 0;
                if (want == _liveOnDriver)
                    return true;

                IntPtr inBuf = Marshal.AllocHGlobal(sizeof(int));
                try
                {
                    Marshal.WriteInt32(inBuf, want ? 1 : 0);

                    bool ok = DeviceIoControl(
                        _handle,
                        IoctlSetLiveEnabled,
                        inBuf,
                        sizeof(int),
                        IntPtr.Zero,
                        0,
                        out _,
                        IntPtr.Zero);

                    if (ok)
                        _liveOnDriver = want;
                    return ok;
                }
                finally
                {
                    Marshal.FreeHGlobal(inBuf);
                }
            }
        }

        public bool TryGetDebugMode(out bool enabled)
        {
            lock (_ioLock)
            {
                enabled = false;
                if (_handle == null || _handle.IsInvalid)
                    return false;

                IntPtr outBuf = Marshal.AllocHGlobal(sizeof(int));
                try
                {
                    bool ok = DeviceIoControl(
                        _handle,
                        IoctlGetDebugMode,
                        IntPtr.Zero,
                        0,
                        outBuf,
                        sizeof(int),
                        out uint bytesReturned,
                        IntPtr.Zero);

                    if (!ok || bytesReturned < sizeof(int))
                        return false;

                    enabled = Marshal.ReadInt32(outBuf) != 0;
                    return true;
                }
                finally
                {
                    Marshal.FreeHGlobal(outBuf);
                }
            }
        }

        public bool SetDebugMode(bool enabled)
        {
            lock (_ioLock)
            {
                if (_handle == null || _handle.IsInvalid)
                    return false;

                IntPtr inBuf = Marshal.AllocHGlobal(sizeof(int));
                try
                {
                    Marshal.WriteInt32(inBuf, enabled ? 1 : 0);

                    return DeviceIoControl(
                        _handle,
                        IoctlSetDebugMode,
                        inBuf,
                        sizeof(int),
                        IntPtr.Zero,
                        0,
                        out _,
                        IntPtr.Zero);
                }
                finally
                {
                    Marshal.FreeHGlobal(inBuf);
                }
            }
        }

        public bool TryGetLiveFrame(out LiveFrame frame)
        {
            frame = default;

            lock (_ioLock)
            {
                if (!IsConnected || _liveFrameBuffer == IntPtr.Zero)
                    return false;

                bool ok = DeviceIoControl(
                    _handle!,
                    IoctlGetLiveFrame,
                    IntPtr.Zero,
                    0,
                    _liveFrameBuffer,
                    (uint)_liveFrameBufferSize,
                    out uint bytesReturned,
                    IntPtr.Zero);

                if (!ok || bytesReturned < (uint)_liveFrameBufferSize)
                {
                    // The driver may no longer be streaming for this handle;
                    // make the next SetLiveConsumer re-send (on or off)
                    // instead of trusting a stale answer.
                    _liveOnDriver = null;
                    return false;
                }

                frame = Marshal.PtrToStructure<LiveFrame>(_liveFrameBuffer);
                return frame.StructVersion == 3;
            }
        }

        public void Dispose() =>
            Disconnect();

        // ---- P/Invoke plumbing ---------------------------------------------

        [DllImport(
            "kernel32.dll",
            SetLastError = true,
            CharSet = CharSet.Auto)]
        private static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport(
            "kernel32.dll",
            SetLastError = true)]
        private static extern bool DeviceIoControl(
            SafeFileHandle device,
            uint ioControlCode,
            IntPtr inBuffer,
            uint inBufferSize,
            IntPtr outBuffer,
            uint outBufferSize,
            out uint bytesReturned,
            IntPtr overlapped);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LiveContact
    {
        public uint ContactID;
        public ushort X;       // normalized X
        public ushort Y;       // normalized Y
        public uint Phase;
        public byte Confident;
        public byte PalmSuspect;
        public ushort Reserved;
        public short RawX;     // exact raw USB abs_x
        public short RawY;     // exact raw USB abs_y
        public ushort Major;   // touch_major, raw sensor units
        public ushort Minor;   // touch_minor, raw sensor units
        public ushort Pressure; // raw pressure/force value
        public ushort Orientation; // raw Apple orientation
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LiveFrame
    {
        public uint StructVersion;
        public uint Sequence;
        public long TimestampQpc;
        public byte ContactCount;
        public byte RawContactCount;
        public byte LargePalmBlanked;
        public byte ButtonDown;
        public byte ForceTouchClick;
        public byte ButtonClickReport;
        public ushort Reserved0;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 5)]
        public LiveContact[] Contacts;
    }

    /// <summary>
    /// Minimal SafeHandle for a Win32 file handle from CreateFile.
    /// </summary>
    internal sealed class SafeFileHandle :
        Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid
    {
        public SafeFileHandle()
            : base(true)
        {
        }

        protected override bool ReleaseHandle() =>
            CloseHandle(handle);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(
            IntPtr handle);
    }
}