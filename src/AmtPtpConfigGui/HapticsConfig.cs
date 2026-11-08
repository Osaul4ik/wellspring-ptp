namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Pressure-haptics ("second click") settings. Unlike Palm/Pointer/Scroll
    /// these are not driver configuration: they belong to the WellspringHaptics
    /// helper, which the GUI pushes them to (see HapticsClient). They still live
    /// in the profile so Save / Reset / profile switching / import / export
    /// treat them like every other setting.
    /// </summary>
    public sealed class HapticsConfig
    {
        public const int ThresholdMin = 50;
        public const int ThresholdMax = 1000;
        public const int ThresholdStep = 5;
        public const int WaveformLight = 0;
        public const int WaveformFirm = 1;

        public bool Enabled { get; set; } = true;
        public int Threshold { get; set; } = 265;
        public int Waveform { get; set; } = WaveformFirm;

        public static HapticsConfig Default => new HapticsConfig();

        public HapticsConfig Clamped()
        {
            int threshold = System.Math.Clamp(Threshold, ThresholdMin, ThresholdMax);
            threshold = (int)System.Math.Round(threshold / (double)ThresholdStep) * ThresholdStep;

            return new HapticsConfig
            {
                Enabled = Enabled,
                Threshold = threshold,
                Waveform = Waveform == WaveformLight ? WaveformLight : WaveformFirm,
            };
        }
    }
}
