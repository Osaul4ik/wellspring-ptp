using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PointerConfig
    {
        public uint StructVersion;
        public uint ForceTapThreshold;
        public uint ForceTapAction;
        public uint ForceTouchEnabled;
        public uint RequirePressureToActivate;
        public uint RequirePressureContinuously;
        public uint CursorSmoothingPercent;
        public uint CursorSpeedPercent;
        public uint CursorDeadzone;
        public uint CursorDeadzoneSlow;
        public uint CursorDeadzoneFast;
        public uint CursorSlowVelocity;
        public uint CursorFastVelocity;
        public uint SmoothingAlphaDen;
        public uint SmoothingAlphaNumSlow;
        // --- Small-contact size filtering ---------------------------------
        // Two INDEPENDENT gates, each with its own on/off flag and its own
        // GUI-tunable "M" (Major axis) threshold. Neither depends on the
        // other being enabled, and both are available for non-Force-Touch
        // AND Force-Touch devices. See the matching AMT_POINTER_CONFIG
        // fields/comments in Public.h.
        public uint SmallContactRejectionEnabled;           // birth-only: on/off
        public uint SmallContactBirthMajorThreshold;         // birth-only: M
        public uint SmallContactRejectionStrict;             // continuous: on/off
        public uint SmallContactMajorThreshold;              // continuous: M

        public uint ForceTouchSmallContactRejectionEnabled;      // birth-only: on/off
        public uint ForceTouchSmallContactBirthMajorThreshold;   // birth-only: M
        public uint ForceTouchSmallContactRejectionStrict;       // continuous: on/off
        public uint ForceTouchSmallContactMajorThreshold;        // continuous: M

        // Software Force Touch emulation (non-Force-Touch trackpads only -
        // see the matching AMT_POINTER_CONFIG fields in Public.h).
        public uint ForceTouchEmulationEnabled;
        public uint ForceTouchEmulationAction;
        public uint ForceTouchEmulationHoldMs;

        // Drag-cancel distance (raw sensor units) - independent per path,
        // see the matching AMT_POINTER_CONFIG fields in Public.h for why.
        public uint ForceTapDragLockoutDistance;
        public uint ForceTouchEmulationDragLockoutDistance;

        // Force Touch action (hard-press -> Force Tap action). ForceTouchEnabled
        // is the master switch; with it on, this flag, RequirePressureToActivate
        // and RequirePressureContinuously are three independent options. See the
        // matching AMT_POINTER_CONFIG field in Public.h.
        public uint ForceTouchActionEnabled;

        // Second haptic click, played by the touchpad DRIVER itself at the
        // moment of a Force Tap (so it works without this app running). The
        // threshold is ForceTapThreshold. See the matching fields in Public.h.
        public uint ForceTouchHapticEnabled;
        public uint ForceTouchHapticWaveform;

        // 1: movement past ForceTapDragLockoutDistance cancels Force Tap and
        // the haptic click; 0: drag cancel is off. See Public.h.
        public uint ForceTapDragCancelEnabled;

        // 14: ForceTouchHardTapHoldMs removed; haptic fields added.
        public const uint CurrentVersion = 14;
        public const uint HapticWaveformMax = 1;
        public const uint ActionContextMenu = 0;
        public const uint ActionMiddleClick = 1;
        public const uint ActionDoubleClick = 2;
        public const uint ThresholdMin = 50;
        public const uint ThresholdMax = 1000;
        public const uint ActionMax = 2;
        public const uint EmulationHoldMsMin = 200;
        public const uint EmulationHoldMsMax = 2000;
        public const uint EmulationHoldMsStep = 50;
        public const uint DragLockoutDistanceMin = 40;
        public const uint DragLockoutDistanceMax = 400;
        public const uint DragLockoutDistanceStep = 10;

        // "M" (Major axis) small-contact cutoff range - shared by the
        // non-Force-Touch and Force-Touch sliders.
        public const uint SmallContactMajorMin = 10;
        public const uint SmallContactMajorMax = 150;

        public static PointerConfig Default => new PointerConfig
        {
            StructVersion = CurrentVersion,
            ForceTapThreshold = 270,
            ForceTapAction = ActionContextMenu,
            ForceTouchEnabled = 1,
            RequirePressureToActivate = 1,
            RequirePressureContinuously = 0,
            CursorSmoothingPercent = 0,
            CursorSpeedPercent = 100,
            CursorDeadzone = 1,
            CursorDeadzoneSlow = 4,
            CursorDeadzoneFast = 0,
            CursorSlowVelocity = 110,
            CursorFastVelocity = 905,
            SmoothingAlphaDen = 8,
            SmoothingAlphaNumSlow = 3,
            SmallContactRejectionEnabled = 1,
            SmallContactBirthMajorThreshold = 80,
            SmallContactRejectionStrict = 1,
            SmallContactMajorThreshold = 10,
            ForceTouchSmallContactRejectionEnabled = 1,
            ForceTouchSmallContactBirthMajorThreshold = 45,
            ForceTouchSmallContactRejectionStrict = 0,
            ForceTouchSmallContactMajorThreshold = 31,
            ForceTouchEmulationEnabled = 1,
            ForceTouchEmulationAction = ActionContextMenu,
            ForceTouchEmulationHoldMs = 300,
            ForceTapDragLockoutDistance = 130,
            ForceTouchEmulationDragLockoutDistance = 110,
            ForceTouchActionEnabled = 1,
            ForceTouchHapticEnabled = 1,
            ForceTouchHapticWaveform = 0,
            ForceTapDragCancelEnabled = 1,
        };

        /// <summary>
        /// Brings a config saved by any earlier build (profile store, profile
        /// file, backup) up to CurrentVersion: fields that did not exist in
        /// the saved version arrive as zero and get the current defaults, then
        /// everything is clamped. A config that was never saved
        /// (StructVersion 0) becomes Default.
        ///
        /// This is the ONE place that knows the version history. It used to be
        /// written out twice (profile store and file/backup loading) and the
        /// two copies had drifted apart - one of them forgot the fields added
        /// in versions 8 and 9, so such a profile came back with the Force
        /// Touch emulation off and a 40-unit drag-cancel distance.
        /// </summary>
        /// <param name="cfg">The config as deserialized.</param>
        /// <param name="legacyHaptics">
        /// The "Haptics" object stored next to it by builds in which this app
        /// played the second click itself, or null if there was none.
        /// </param>
        public static PointerConfig Migrate(PointerConfig cfg, HapticsConfig? legacyHaptics = null)
        {
            if (cfg.StructVersion == 0)
                return Default;

            if (cfg.StructVersion < CurrentVersion)
            {
                PointerConfig defaults = Default;

                if (cfg.StructVersion < 3)
                {
                    cfg.CursorDeadzoneSlow = defaults.CursorDeadzoneSlow;
                    cfg.CursorDeadzoneFast = defaults.CursorDeadzoneFast;
                    cfg.SmoothingAlphaDen = defaults.SmoothingAlphaDen;
                    cfg.SmoothingAlphaNumSlow = defaults.SmoothingAlphaNumSlow;
                }

                // 3 -> 4: Force Touch master switch and the pressure gate.
                if (cfg.StructVersion < 4)
                {
                    cfg.ForceTouchEnabled = defaults.ForceTouchEnabled;
                    cfg.RequirePressureToActivate = defaults.RequirePressureToActivate;
                }

                // 4 -> 5: non-Force-Touch small-contact rejection.
                if (cfg.StructVersion < 5)
                    cfg.SmallContactRejectionEnabled = defaults.SmallContactRejectionEnabled;

                // 5 -> 6: the continuous small-contact gate.
                if (cfg.StructVersion < 6)
                    cfg.SmallContactRejectionStrict = defaults.SmallContactRejectionStrict;

                // 6 -> 7: the continuous Force Touch pressure gate.
                if (cfg.StructVersion < 7)
                    cfg.RequirePressureContinuously = defaults.RequirePressureContinuously;

                // 7 -> 8: Force Touch emulation.
                if (cfg.StructVersion < 8)
                {
                    cfg.ForceTouchEmulationEnabled = defaults.ForceTouchEmulationEnabled;
                    cfg.ForceTouchEmulationAction = defaults.ForceTouchEmulationAction;
                    cfg.ForceTouchEmulationHoldMs = defaults.ForceTouchEmulationHoldMs;
                }

                // 8 -> 9: the two drag-cancel distances.
                if (cfg.StructVersion < 9)
                {
                    cfg.ForceTapDragLockoutDistance = defaults.ForceTapDragLockoutDistance;
                    cfg.ForceTouchEmulationDragLockoutDistance = defaults.ForceTouchEmulationDragLockoutDistance;
                }

                // 9 -> 10: tunable continuous-gate "M" threshold, and the
                // mirrored small-contact gate for Force Touch devices.
                if (cfg.StructVersion < 10)
                {
                    cfg.SmallContactMajorThreshold = defaults.SmallContactMajorThreshold;
                    cfg.ForceTouchSmallContactRejectionEnabled = defaults.ForceTouchSmallContactRejectionEnabled;
                    cfg.ForceTouchSmallContactRejectionStrict = defaults.ForceTouchSmallContactRejectionStrict;
                    cfg.ForceTouchSmallContactMajorThreshold = defaults.ForceTouchSmallContactMajorThreshold;
                }

                // 10 -> 11: birth-only gates get their own "M" thresholds.
                if (cfg.StructVersion < 11)
                {
                    cfg.SmallContactBirthMajorThreshold = defaults.SmallContactBirthMajorThreshold;
                    cfg.ForceTouchSmallContactBirthMajorThreshold = defaults.ForceTouchSmallContactBirthMajorThreshold;
                }

                // 11 -> 12: the Force Touch action became its own switch.
                if (cfg.StructVersion < 12)
                    cfg.ForceTouchActionEnabled = defaults.ForceTouchActionEnabled;

                // 13 -> 14: the hard-tap hold time is gone and the second
                // haptic click moved into the driver. If this app was playing
                // it for this profile, carry its on/off switch and waveform
                // over so the driver keeps doing what the app did; otherwise
                // adopt the defaults. The pressure threshold stays the
                // config's own ForceTapThreshold (see HapticsConfig.Threshold).
                if (cfg.StructVersion < 14)
                {
                    if (legacyHaptics != null)
                    {
                        cfg.ForceTouchHapticEnabled = legacyHaptics.Enabled ? 1u : 0u;
                        cfg.ForceTouchHapticWaveform =
                            legacyHaptics.Waveform == HapticsConfig.WaveformFirm ? 1u : 0u;
                    }
                    else
                    {
                        cfg.ForceTouchHapticEnabled = defaults.ForceTouchHapticEnabled;
                        cfg.ForceTouchHapticWaveform = defaults.ForceTouchHapticWaveform;
                    }

                    cfg.ForceTapDragCancelEnabled = defaults.ForceTapDragCancelEnabled;
                }
            }

            return cfg.Clamped();
        }

        public PointerConfig Clamped()
        {
            PointerConfig c = this;
            c.StructVersion = CurrentVersion;
            c.ForceTapThreshold = Clamp(c.ForceTapThreshold, ThresholdMin, ThresholdMax);
            if (c.ForceTapAction > ActionMax) c.ForceTapAction = ActionContextMenu;
            c.ForceTouchEnabled = c.ForceTouchEnabled != 0 ? 1u : 0u;
            c.RequirePressureToActivate = c.RequirePressureToActivate != 0 ? 1u : 0u;
            c.RequirePressureContinuously = c.RequirePressureContinuously != 0 ? 1u : 0u;
            c.ForceTouchActionEnabled = c.ForceTouchActionEnabled != 0 ? 1u : 0u;
            c.ForceTouchHapticEnabled = c.ForceTouchHapticEnabled != 0 ? 1u : 0u;
            if (c.ForceTouchHapticWaveform > HapticWaveformMax) c.ForceTouchHapticWaveform = HapticWaveformMax;
            c.ForceTapDragCancelEnabled = c.ForceTapDragCancelEnabled != 0 ? 1u : 0u;
            // Two INDEPENDENT small-contact gates per device kind - neither
            // flag forces the other off. Each has its own "M" threshold.
            c.SmallContactRejectionEnabled = c.SmallContactRejectionEnabled != 0 ? 1u : 0u;
            c.SmallContactBirthMajorThreshold = Clamp(c.SmallContactBirthMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.SmallContactRejectionStrict = c.SmallContactRejectionStrict != 0 ? 1u : 0u;
            c.SmallContactMajorThreshold = Clamp(c.SmallContactMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);

            c.ForceTouchSmallContactRejectionEnabled = c.ForceTouchSmallContactRejectionEnabled != 0 ? 1u : 0u;
            c.ForceTouchSmallContactBirthMajorThreshold = Clamp(c.ForceTouchSmallContactBirthMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.ForceTouchSmallContactRejectionStrict = c.ForceTouchSmallContactRejectionStrict != 0 ? 1u : 0u;
            c.ForceTouchSmallContactMajorThreshold = Clamp(c.ForceTouchSmallContactMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.CursorSmoothingPercent = Clamp(c.CursorSmoothingPercent, 0, 100);
            c.CursorSpeedPercent = Clamp(c.CursorSpeedPercent, 50, 200);
            c.CursorDeadzone = Clamp(c.CursorDeadzone, 0, 8);
            c.CursorDeadzoneSlow = Clamp(c.CursorDeadzoneSlow, 0, 8);
            c.CursorDeadzoneFast = Clamp(c.CursorDeadzoneFast, 0, 8);
            c.CursorSlowVelocity = Clamp(c.CursorSlowVelocity, 20, 300);
            c.CursorFastVelocity = Clamp(c.CursorFastVelocity, 200, 2000);
            if (c.CursorFastVelocity <= c.CursorSlowVelocity) c.CursorFastVelocity = c.CursorSlowVelocity + 1;
            c.SmoothingAlphaDen = Clamp(c.SmoothingAlphaDen, 1, 16);
            c.SmoothingAlphaNumSlow = Clamp(c.SmoothingAlphaNumSlow, 1, 16);
            if (c.SmoothingAlphaNumSlow > c.SmoothingAlphaDen) c.SmoothingAlphaNumSlow = c.SmoothingAlphaDen;

            c.ForceTouchEmulationEnabled = c.ForceTouchEmulationEnabled != 0 ? 1u : 0u;
            if (c.ForceTouchEmulationAction > ActionMax) c.ForceTouchEmulationAction = ActionContextMenu;
            c.ForceTouchEmulationHoldMs = ClampToGrid(c.ForceTouchEmulationHoldMs, EmulationHoldMsMin, EmulationHoldMsMax, EmulationHoldMsStep);

            c.ForceTapDragLockoutDistance = ClampToGrid(c.ForceTapDragLockoutDistance, DragLockoutDistanceMin, DragLockoutDistanceMax, DragLockoutDistanceStep);

            c.ForceTouchEmulationDragLockoutDistance = ClampToGrid(c.ForceTouchEmulationDragLockoutDistance, DragLockoutDistanceMin, DragLockoutDistanceMax, DragLockoutDistanceStep);

            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);

        // Clamp, then re-quantize onto the Step grid the corresponding GUI
        // slider steps in (round to nearest, then clamp again in case
        // rounding pushed an edge value one step out of range). Mirrors
        // AmtClampToStepGrid in the driver's ConfigIoctl.c - replaces three
        // near-identical clamp/round/clamp blocks that used to live inline
        // here.
        private static uint ClampToGrid(uint v, uint min, uint max, uint step)
        {
            v = Clamp(v, min, max);
            v = ((v + step / 2) / step) * step;
            return Clamp(v, min, max);
        }
    }
}