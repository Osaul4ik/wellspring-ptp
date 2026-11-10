namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// LEGACY, read-only. Before the second haptic click moved into the
    /// touchpad driver it was played by this app, and its settings lived in a
    /// separate "Haptics" object next to "Pointer" in profiles, profile files
    /// and backups. They are ordinary driver configuration now
    /// (PointerConfig.ForceTouchHapticEnabled / ForceTouchHapticWaveform, with
    /// ForceTapThreshold as the one pressure threshold), so nothing writes
    /// this object any more. It only exists so that an old file still
    /// deserializes and PointerConfig.Migrate can carry the on/off switch and
    /// the waveform over.
    /// </summary>
    public sealed class HapticsConfig
    {
        public const int WaveformLight = 0;
        public const int WaveformFirm = 1;

        public bool Enabled { get; set; } = true;

        // Read from old files but deliberately not migrated: the driver has
        // one pressure threshold (PointerConfig.ForceTapThreshold), and a
        // profile's own Force Touch threshold must not be replaced by a haptic
        // threshold the user may never have touched.
        public int Threshold { get; set; } = 270;

        public int Waveform { get; set; } = WaveformLight;
    }
}
