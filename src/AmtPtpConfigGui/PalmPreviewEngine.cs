using AmtPtpConfigGui.Native;

namespace AmtPtpConfigGui
{
    public enum PalmClass
    {
        None = 0,
        Local = 1, // soft/edge reject - "PALM_LOCAL" in Palm.c
        Large = 2, // hard reject, blanks the whole pad - "PALM_LARGE" in Palm.c
    }

    /// <summary>
    /// Line-for-line port of AmtPalmClassify / AmtPalmInEdgeZone from
    /// src/AmtPtpDeviceUsbKm/Palm.c, so the GUI's live preview shows exactly
    /// what the kernel driver would decide for a given
    /// (Major, Minor, X, Y, IsBirth, PalmConfig) - without needing a
    /// connected device. Keep this in sync with Palm.c if the algorithm
    /// there ever changes; there is intentionally no shared source file
    /// between the kernel driver and this WPF app (different runtimes), so
    /// this is a deliberate, documented duplication.
    /// </summary>
    public static class PalmPreviewEngine
    {
        public static bool InEdgeZone(PadGeometry geo, PalmConfig cfg, double x, double y)
        {
            double xRange = geo.XMax - geo.XMin;
            double yRange = geo.YMax - geo.YMin;

            double edgeLeft = xRange * cfg.EdgePermilleLeft / 1000.0;
            double edgeRight = xRange * cfg.EdgePermilleRight / 1000.0;
            double edgeTop = yRange * cfg.EdgePermilleTop / 1000.0;
            double edgeBottom = yRange * cfg.EdgePermilleBottom / 1000.0;

            return x < edgeLeft || x > (xRange - edgeRight) ||
                   y < edgeTop || y > (yRange - edgeBottom);
        }

        public static PalmClass Classify(
            PadGeometry geo, PalmConfig cfg,
            int major, int minor, double x, double y, bool isBirth,
            bool gestureContext = false)
        {
            // gestureContext: other fingers are down (gesture in progress or
            // forming). The edge zone then does not hard-reject a birth; the
            // contact is judged by shape/score only (see Palm.c).
            bool inEdge = false;
            bool edgeKnown = false;
            if (isBirth)
            {
                inEdge = InEdgeZone(geo, cfg, x, y);
                edgeKnown = true;
                if (inEdge && !gestureContext)
                {
                    return PalmClass.Local;
                }
            }

            if (major < cfg.PalmMinMajor && minor < cfg.PalmMinMinor)
            {
                return PalmClass.None;
            }

            int score = 0;

            if (major <= 0 && minor <= 0)
                return PalmClass.None;

            if (major >= cfg.PalmLargeMajor)
            {
                if (minor <= 0)
                    return PalmClass.Large;

                if ((long)major * 100 >= (long)(cfg.PalmLargeRatio + 1) * minor)
                    return PalmClass.Large;
            }

            if (major > 300) score += 35;
            else if (major > 220) score += 15;
            else if (major > 150) score += 8;

            if (minor > 0 && major > 120)
            {
                long major100 = (long)major * 100;
                if (major100 >= 1201L * minor) score += 30;
                else if (major100 >= 901L * minor) score += 20;
                else if (major100 >= 601L * minor) score += 10;
            }

            if (major > 130)
            {
                if (!edgeKnown)
                    inEdge = InEdgeZone(geo, cfg, x, y);
                if (inEdge)
                    score += 10;
            }

            return score >= cfg.PalmScoreThresh ? PalmClass.Local : PalmClass.None;
        }
    }
}