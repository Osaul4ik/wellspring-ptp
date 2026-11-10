using System.Text.Json.Serialization;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// On-disk format for the GUI profile save/load (.json)
    /// buttons. Bundles PalmConfig, ScrollConfig and PointerConfig into one file so
    /// a single Save/Load round-trips everything the global bottom-bar
    /// Save/Reset buttons touch - not just Palm. The haptic click's settings
    /// are part of PointerConfig.
    ///
    /// ProfileFormatVersion distinguishes this shape from the legacy
    /// (pre-Pointer-support) file format, which was just a bare PalmConfig
    /// JSON object with no wrapper - see MainWindow.LoadProfile_Click for
    /// the format-sniffing fallback that still loads those old files.
    /// </summary>
    public sealed class AmtPtpProfile
    {
        // 4: the separate "Haptics" object is no longer written (its settings
        // live in Pointer). Reading is unaffected - see Haptics below.
        public const int CurrentFormatVersion = 4;

        public int ProfileFormatVersion { get; set; } = CurrentFormatVersion;
        public PalmConfig Palm { get; set; }
        public PointerConfig Pointer { get; set; }
        public ScrollConfig Scroll { get; set; }

        // LEGACY, read-only: present in files written while this app played
        // the second click itself. Only consumed by PointerConfig.Migrate;
        // never set by FromCurrent, so it is not written back.
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public HapticsConfig? Haptics { get; set; }

        public static AmtPtpProfile FromCurrent(PalmConfig palm, PointerConfig pointer, ScrollConfig scroll) =>
            new AmtPtpProfile
            {
                ProfileFormatVersion = CurrentFormatVersion,
                Palm = palm,
                Pointer = pointer,
                Scroll = scroll,
            };
    }
}
