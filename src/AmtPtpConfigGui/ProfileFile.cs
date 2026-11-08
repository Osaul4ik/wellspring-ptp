namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// On-disk format for the GUI profile save/load (.json)
    /// buttons. Bundles PalmConfig, ScrollConfig, PointerConfig and HapticsConfig into one file so
    /// a single Save/Load round-trips everything the global bottom-bar
    /// Save/Reset buttons touch - not just Palm.
    ///
    /// ProfileFormatVersion distinguishes this shape from the legacy
    /// (pre-Pointer-support) file format, which was just a bare PalmConfig
    /// JSON object with no wrapper - see MainWindow.LoadProfile_Click for
    /// the format-sniffing fallback that still loads those old files.
    /// </summary>
    public sealed class AmtPtpProfile
    {
        public const int CurrentFormatVersion = 3;

        public int ProfileFormatVersion { get; set; } = CurrentFormatVersion;
        public PalmConfig Palm { get; set; }
        public PointerConfig Pointer { get; set; }
        public ScrollConfig Scroll { get; set; }

        // Optional: files written before haptics existed have no entry, and
        // loading one must leave the current haptic settings alone.
        public HapticsConfig? Haptics { get; set; }

        public static AmtPtpProfile FromCurrent(PalmConfig palm, PointerConfig pointer, ScrollConfig scroll, HapticsConfig haptics) =>
            new AmtPtpProfile
            {
                ProfileFormatVersion = CurrentFormatVersion,
                Palm = palm,
                Pointer = pointer,
                Scroll = scroll,
                Haptics = haptics,
            };
    }
}