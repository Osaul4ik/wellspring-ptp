using System;
using System.Runtime.InteropServices;
using ActuatorHandle = Microsoft.Win32.SafeHandles.SafeFileHandle;

namespace AmtPtpConfigGui
{
    /// <summary>
    /// User-mode access to the haptic actuator driver
    /// (src/AmtHapticDeviceUsbKm). Mirrors its Public.h: keep both in sync and
    /// bump AMT_HAPTIC_INTERFACE_VERSION there and InterfaceVersion here on any
    /// layout change.
    /// </summary>
    internal static class HapticsActuator
    {
        public enum FindResult
        {
            Found,
            NotInstalled,
            Multiple,
            Error
        }

        // Must match GUID_DEVINTERFACE_AMT_HAPTIC in Public.h.
        private static readonly Guid InterfaceGuid = new Guid("15e33c40-8ec9-4ab3-afe1-b3e6215f4499");

        // Must match AMT_HAPTIC_INTERFACE_VERSION in Public.h.
        private const uint InterfaceVersion = 1;

        // CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
        // and + 1 with FILE_WRITE_DATA: IOCTL_AMT_HAPTIC_GET_INFO / _CLICK.
        private const uint IoctlGetInfo = 0x00226000;
        private const uint IoctlClick = 0x0022A004;

        // sizeof(AMT_HAPTIC_INFO) and sizeof(AMT_HAPTIC_CLICK_REQUEST).
        private const int InfoSize = 48;
        private const uint ClickRequestSize = 8;

        // The driver's own 100 ms click cooldown (STATUS_DEVICE_BUSY) surfaces
        // as ERROR_BUSY. It is flow control, not a transport failure.
        public const int ErrorBusy = 170;

        private const uint DigcfPresent = 0x02;
        private const uint DigcfDeviceInterface = 0x10;
        private const uint GenericReadWrite = 0xC0000000;
        private const uint FileShareReadWrite = 3;
        private const uint OpenExisting = 3;

        [StructLayout(LayoutKind.Sequential)]
        private struct DeviceInterfaceData
        {
            public int Size;
            public Guid InterfaceClassGuid;
            public int Flags;
            public IntPtr Reserved;
        }

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, string? enumerator, IntPtr parent, uint flags);

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr info, ref Guid classGuid, uint index, ref DeviceInterfaceData data);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref DeviceInterfaceData data, IntPtr detail, uint size, out uint required, IntPtr info);

        [DllImport("setupapi.dll")]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern ActuatorHandle CreateFile(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(ActuatorHandle handle, uint code, ref ClickRequest input, uint inputSize, IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(ActuatorHandle handle, uint code, IntPtr input, uint inputSize, IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);

        [StructLayout(LayoutKind.Sequential)]
        private struct ClickRequest
        {
            public uint StructVersion;
            public uint Waveform;
        }

        /// <summary>The driver's AMT_HAPTIC_INFO (diagnostics and version).</summary>
        public readonly record struct Info(
            uint StructVersion,
            uint WaveformCount,
            uint MinClickIntervalMs,
            uint ClicksDelivered,
            uint ClicksThrottled,
            uint ClicksFailed,
            int LastStatus);

        /// <summary>
        /// Locates the single bound actuator interface. A second match is
        /// refused rather than guessed at: the driver only binds MI_03 of one
        /// trackpad.
        /// </summary>
        public static FindResult Find(out string? path)
        {
            path = null;
            Guid guid = InterfaceGuid;
            IntPtr set = SetupDiGetClassDevs(ref guid, null, IntPtr.Zero, DigcfPresent | DigcfDeviceInterface);
            if (set == IntPtr.Zero || set == new IntPtr(-1))
                return FindResult.Error;

            try
            {
                var first = new DeviceInterfaceData { Size = Marshal.SizeOf<DeviceInterfaceData>() };
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, 0, ref first))
                    return FindResult.NotInstalled;

                var second = new DeviceInterfaceData { Size = first.Size };
                if (SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, 1, ref second))
                    return FindResult.Multiple;

                SetupDiGetDeviceInterfaceDetail(set, ref first, IntPtr.Zero, 0, out uint needed, IntPtr.Zero);
                if (needed < 6)
                    return FindResult.Error;

                IntPtr buffer = Marshal.AllocHGlobal((int)needed);
                try
                {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize is the fixed
                    // header size: 8 on 64-bit (padded), 6 on 32-bit.
                    Marshal.WriteInt32(buffer, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref first, buffer, needed, out needed, IntPtr.Zero))
                        return FindResult.Error;

                    path = Marshal.PtrToStringUni(IntPtr.Add(buffer, 4));
                    return path == null ? FindResult.Error : FindResult.Found;
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(set);
            }
        }

        /// <summary>
        /// Null when the interface cannot be opened or its driver speaks a
        /// different interface version than this build: a client and a driver
        /// from different revisions must not exchange buffers.
        /// </summary>
        public static ActuatorHandle? Open(string path)
        {
            ActuatorHandle handle = CreateFile(path, GenericReadWrite, FileShareReadWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
            if (handle.IsInvalid)
            {
                handle.Dispose();
                return null;
            }

            if (!TryGetInfo(handle, out Info info) || info.StructVersion != InterfaceVersion)
            {
                handle.Dispose();
                return null;
            }

            return handle;
        }

        public static bool TryGetInfo(ActuatorHandle handle, out Info info)
        {
            info = default;
            IntPtr buffer = Marshal.AllocHGlobal(InfoSize);
            try
            {
                if (!DeviceIoControl(handle, IoctlGetInfo, IntPtr.Zero, 0, buffer, InfoSize, out uint returned, IntPtr.Zero)
                    || returned < InfoSize)
                {
                    return false;
                }

                info = new Info(
                    (uint)Marshal.ReadInt32(buffer, 0),
                    (uint)Marshal.ReadInt32(buffer, 4),
                    (uint)Marshal.ReadInt32(buffer, 8),
                    (uint)Marshal.ReadInt32(buffer, 12),
                    (uint)Marshal.ReadInt32(buffer, 16),
                    (uint)Marshal.ReadInt32(buffer, 20),
                    Marshal.ReadInt32(buffer, 24));
                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        /// <summary>
        /// waveform: 0 = light, 1 = firm (AMT_HAPTIC_WAVEFORM_*). Returns 0 on
        /// success, otherwise the Win32 error of the failed IOCTL.
        /// </summary>
        public static int Click(ActuatorHandle handle, int waveform)
        {
            var request = new ClickRequest
            {
                StructVersion = InterfaceVersion,
                Waveform = waveform == 1 ? 1u : 0u,
            };

            if (DeviceIoControl(handle, IoctlClick, ref request, ClickRequestSize, IntPtr.Zero, 0, out _, IntPtr.Zero))
                return 0;

            return Marshal.GetLastWin32Error();
        }
    }
}
