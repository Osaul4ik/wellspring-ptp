// ActiveContact.c - Contact lifecycle FSM.

#include "Driver.h"
#include "ActiveContact.h"

// Pointer/scroll thresholds, deadzones, smoothing coefficients and scroll
// scale factors are runtime-configurable through AMT_*_CONFIG.

typedef enum _CONTACT_VELOCITY_BUCKET
{
    VELOCITY_UNKNOWN = 0,  // no prior timestamp
    VELOCITY_SLOW,
    VELOCITY_MEDIUM,
    VELOCITY_FAST,
} CONTACT_VELOCITY_BUCKET;

static inline INT
AmtMulQ32Round(_In_ INT Value, _In_ LONGLONG CoeffQ32)
{
    LONGLONG product = (LONGLONG)Value * CoeffQ32;
    const LONGLONG half = 1LL << (AMT_RUNTIME_FIXED_SHIFT - 1);

    if (product >= 0)
        return (INT)((product + half) >> AMT_RUNTIME_FIXED_SHIFT);

    product = -product;
    return -(INT)((product + half) >> AMT_RUNTIME_FIXED_SHIFT);
}

static inline INT
AmtQ32ProductToIntRound(_In_ LONGLONG Product)
{
    const LONGLONG half = 1LL << (AMT_RUNTIME_FIXED_SHIFT - 1);

    if (Product >= 0)
        return (INT)((Product + half) >> AMT_RUNTIME_FIXED_SHIFT);

    Product = -Product;
    return -(INT)((Product + half) >> AMT_RUNTIME_FIXED_SHIFT);
}

// Lower smoothing reduces slow-speed jitter.

// Continuous alpha endpoints and scroll scale coefficients are supplied by
// precomputed runtime state; raw config remains the authoritative user-facing
// representation.

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal, _In_ INT alphaNum,
    _In_ INT alphaDen, _In_ LONGLONG invAlphaDenQ32)
{
    // Blend the new sample with the previous report.
    //
    // MICRO-OPT: was `numerator / alphaDen` - a genuine runtime division,
    // per coordinate (X and Y), per active contact, every non-gesture
    // frame - the one spot in the hot path that violated this file's own
    // "interrupt path never performs percent division" invariant.
    // alphaDen is a config-runtime value (SET_POINTER_CONFIG only), so
    // its reciprocal is precomputed once in AmtPointerRuntimeRebuild
    // (CursorSmoothingInvAlphaDenQ32) and reused here via the same
    // multiply+shift pattern (AmtMulQ32Round) already used everywhere
    // else in this file. alphaDen itself is still needed for the
    // (alphaDen - alphaNum) blend weight below - only the division BY
    // alphaDen is replaced.
    INT numerator = (INT)rawVal * alphaNum + (INT)prevVal * (alphaDen - alphaNum);
    INT blended = AmtMulQ32Round(numerator, invAlphaDenQ32);
    return (USHORT)blended;
}

// Estimate motion speed and the continuous smoothing alpha from a single
// distance/unitsPerSec computation, shared between the two consumers below
// instead of each recomputing dx/dy/distance separately.
//
// Bucket uses FastThresholdUnitsPerSec (caller picks cursor vs. scroll
// threshold). Alpha always ramps against the cursor thresholds
// (VELOCITY_SLOW/FAST_UNITS_PER_SEC) regardless of which threshold the
// bucket used - it's only ever consumed by the caller on the non-gesture
// (single-finger) path, but is always computed and written here so
// OutAlphaNum is unconditionally valid on return.
static CONTACT_VELOCITY_BUCKET
AmtContactEvaluateVelocity(
    _In_  USHORT   rawX,
    _In_  USHORT   rawY,
    _In_  USHORT   prevX,
    _In_  USHORT   prevY,
    _In_  LONGLONG DtQpcTicks,
    _In_  LONGLONG PerfFrequencyHz,
    _In_  LONGLONG FastThresholdUnitsPerSec,
    _In_  LONGLONG        SlowThresholdUnitsPerSec,
    _In_  LONGLONG        AlphaFastThresholdUnitsPerSec,
    _In_  const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_  BOOLEAN         ComputeAlpha,
    _Out_ INT*     OutAlphaNum
)
{
    if (DtQpcTicks <= 0 || PerfFrequencyHz <= 0) {
        // No timestamp basis yet - same default as before: raw/unknown.
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
        return VELOCITY_UNKNOWN;
    }

    INT dx = AmtAbsDelta((INT)rawX, (INT)prevX);
    INT dy = AmtAbsDelta((INT)rawY, (INT)prevY);
    INT distance = (dx > dy) ? dx : dy;

    LONGLONG unitsPerSec = ((LONGLONG)distance * PerfFrequencyHz) / DtQpcTicks;

    if (!ComputeAlpha) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
        if (unitsPerSec <= SlowThresholdUnitsPerSec)
            return VELOCITY_SLOW;
        if (unitsPerSec >= FastThresholdUnitsPerSec)
            return VELOCITY_FAST;
        return VELOCITY_MEDIUM;
    }

    CONTACT_VELOCITY_BUCKET bucket;
    if (unitsPerSec <= SlowThresholdUnitsPerSec) {
        bucket = VELOCITY_SLOW;
    } else if (unitsPerSec >= FastThresholdUnitsPerSec) {
        bucket = VELOCITY_FAST;
    } else {
        bucket = VELOCITY_MEDIUM;
    }

    // Continuous ramp between the configured slow alpha numerator (at/below
    // the slow velocity threshold) and the configured denominator (at/above
    // the fast velocity threshold, bit-identical to raw), so a speed
    // anywhere in between gets a proportionally lighter blend instead
    // of snapping between two fixed states at a single threshold.
    //
    // On gesture frames ComputeAlpha is FALSE, so the alpha-ramp division is
    // skipped entirely; the caller does not consume alpha for gestures.
    if (unitsPerSec <= SlowThresholdUnitsPerSec) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaNumSlow;
    } else if (unitsPerSec >= AlphaFastThresholdUnitsPerSec) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
    } else {
        LONGLONG offset = unitsPerSec - SlowThresholdUnitsPerSec;
        INT ramp = AmtMulQ32Round((INT)offset, PointerRuntime->CursorSmoothingSlopeQ32);
        INT alpha = PointerRuntime->CursorSmoothingAlphaNumSlow + ramp;
        if (alpha > PointerRuntime->CursorSmoothingAlphaDen)
            alpha = PointerRuntime->CursorSmoothingAlphaDen;
        *OutAlphaNum = alpha;
    }

    return bucket;
}

static inline INT
AmtContactDeadzoneForVelocity(
    _In_ CONTACT_VELOCITY_BUCKET Velocity,
    _In_ BOOLEAN GestureActive,
    _In_ const AMT_POINTER_CONFIG* PointerConfig,
    _In_ const AMT_SCROLL_CONFIG* ScrollConfig)
{
    if (GestureActive)
        return (INT)ScrollConfig->Deadzone;

    if (Velocity == VELOCITY_SLOW)
        return (INT)PointerConfig->CursorDeadzoneSlow;
    if (Velocity == VELOCITY_FAST)
        return (INT)PointerConfig->CursorDeadzoneFast;
    return (INT)PointerConfig->CursorDeadzone;
}

// Assign a monotonic contact ID.
static inline ULONG
AmtContactAssignId(_Inout_ ULONG* NextContactId)
{
    ULONG id = ++(*NextContactId);
    if (id == 0) {
        // 0 is reserved; skip it.
        id = ++(*NextContactId);
    }
    return id;
}

BOOLEAN
AmtContactPoolFindSensorPos(
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _In_  ULONG   ContactID,
    _Out_ USHORT* SensorX,
    _Out_ USHORT* SensorY
)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        if (Pool[i].State != CONTACT_FREE && Pool[i].ContactID == ContactID) {
            *SensorX = Pool[i].ReportX;
            *SensorY = Pool[i].ReportY;
            return TRUE;
        }
    }
    return FALSE;
}

VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool)
{
    RtlZeroMemory(Pool, sizeof(ACTIVE_CONTACT) * MAX_CONTACTS);
}

size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        if (Pool[i].State == CONTACT_FREE)
            return i;
    }
    return MAX_CONTACTS; // pool exhausted (should not happen)
}

// Shared field init for AmtContactBirth / AmtContactBirthWithRetapSmoothing -
// identical in every field except the X/Y source and RetapSeeded.
static inline VOID
AmtContactBirthCommon(
    _Inout_ PACTIVE_CONTACT c,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          emitX,
    _In_    USHORT          emitY,
    _In_    USHORT          slotHint,
    _In_    BOOLEAN         retapSeeded
)
{
    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    c->ReportX            = x;
    c->ReportY            = y;
    c->HystX              = x;
    c->HystY              = y;
    c->EmitX              = emitX;
    c->EmitY              = emitY;
    c->WasInGesture       = FALSE;
    c->PendingFirstSample = TRUE;
    c->RetapSeeded        = retapSeeded;
    c->CarryX              = 0;
    c->CarryY              = 0;
    c->Announced           = FALSE;
    c->SoloMs              = 0;
    c->HystAgeTicks        = 0;
    c->ScrollFilteredX     = 0;
    c->ScrollFilteredY     = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate
    c->LastMajor           = 0;
    c->LastMinor           = 0;
    c->LastPressure        = 0;
    c->FramesAlive         = 1; // birth frame counts as 1
}

VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    AmtContactBirthCommon(c, NextContactId, x, y, x, y, slotHint, /* retapSeeded */ FALSE);
}

// Seeds EMA baseline so the cursor doesn't jump on re-tap.
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          RecentLiftEmitX,
    _In_    USHORT          RecentLiftEmitY,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    AmtContactBirthCommon(c, NextContactId, RecentLiftX, RecentLiftY,
                           RecentLiftEmitX, RecentLiftEmitY, slotHint,
                           /* retapSeeded */ TRUE); // preserve seed on first update
}

VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE || c->State == CONTACT_GRACE);
#endif

    // Windows-facing position: the UP must land where Windows last saw
    // this contact, not at the sensor-domain smoothed position.
    *OldContactID = c->ContactID;
    *OldX         = c->EmitX;
    *OldY         = c->EmitY;

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

// Synthetic rebirth keeps smoothing state while swapping the contact ID.
VOID
AmtContactRebindIdentity(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _Out_   ULONG*          OldContactID
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE);
#endif

    *OldContactID = c->ContactID;
    c->ContactID  = AmtContactAssignId(NextContactId);
    c->Announced  = FALSE; // the new identity has not been reported yet
}

VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE);
#endif

    *OldContactID = c->ContactID;
    *OldX         = c->EmitX;
    *OldY         = c->EmitY;

    c->State = CONTACT_GRACE;
}

VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_GRACE);
#endif

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY,
    _In_ INT                   ThresholdUnits
)
{
    if (ThresholdUnits <= 0) {
        return TRUE;
    }

    INT dx = AmtAbsDelta((INT)candX, (INT)Contact->HystX);
    INT dy = AmtAbsDelta((INT)candY, (INT)Contact->HystY);

    return (dx >= ThresholdUnits) || (dy >= ThresholdUnits);
}

// Scale a sensor-domain delta by a Q32 factor, carrying the sub-unit
// remainder so the long-run sum of emitted steps equals scale * sum of raw
// deltas exactly (no truncation bias, no lost slow motion).
//
// The carry is the Q32 fractional part left over after flooring, so it is
// always in [0, 2^32). Floor (arithmetic shift) rather than round-to-nearest
// keeps the carry non-negative and the accumulation unbiased for both
// directions. A factor of exactly 1.0 is the identity and leaves the carry
// untouched.
static inline INT
AmtScaleDeltaWithCarry(
    _In_    INT       Delta,
    _In_    LONGLONG  ScaleQ32,
    _Inout_ ULONG*    Carry
)
{
    if (ScaleQ32 == AMT_RUNTIME_FIXED_ONE)
        return Delta;

    LONGLONG sum = (LONGLONG)Delta * ScaleQ32 + (LONGLONG)*Carry;
    INT whole = (INT)(sum >> AMT_RUNTIME_FIXED_SHIFT);
    *Carry = (ULONG)(sum & 0xFFFFFFFFLL);
    return whole;
}

// Clamp to the pad's normalized extent [0, Max]. Scaling by a factor > 1
// (e.g. the default 1.08x fast-scroll boost) integrates to a position that
// can run past the pad edge by up to (scale-1) x the distance travelled.
// The HID descriptor declares a logical maximum, so values beyond it are
// not something to hand Windows: pin the integrated position to the pad.
// Clamping the integrator state itself (not just the output) means motion
// resumes immediately when the finger turns around, instead of first
// having to "unwind" an invisible overshoot.
static inline USHORT
AmtClampToRange(_In_ LONG v, _In_ USHORT Max)
{
    return (USHORT)(v < 0 ? 0 : (v > (LONG)Max ? (LONG)Max : v));
}

static inline VOID
AmtContactCommitSample(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          candX,
    _In_    USHORT          candY,
    _In_    BOOLEAN         passedDeadzone,
    _In_    BOOLEAN         aliveCountIsOne,
    _In_    BOOLEAN         gestureActive,
    _In_    CONTACT_VELOCITY_BUCKET Velocity,
    _In_    INT             alphaNum,
    _In_    BOOLEAN         commitIsRetapSeededFirstSample,
    _In_    const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_    const AMT_SCROLL_RUNTIME* ScrollRuntime,
    _In_    USHORT          padMaxX,
    _In_    USHORT          padMaxY,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
    BOOLEAN velocityIsSlow = (Velocity == VELOCITY_SLOW);

    if (passedDeadzone) {
        // Two coordinate domains live side by side here:
        //
        //   ReportX/Y  sensor domain  - smoothed raw position, used for
        //                               matching; always tracks the finger.
        //   EmitX/Y    Windows domain - integrated from SCALED sensor
        //                               deltas; what the PTP report carries.
        //
        // Scroll scale and cursor speed MUST act on the sensor-domain
        // delta (movement since the previous committed sample) and be
        // integrated into EmitX/Y. Scaling the distance to the previous
        // emitted position instead (the old behaviour) turns the scale
        // into a first-order filter that converges back to the raw
        // position: steady-state speed and total displacement stay equal
        // to the finger's, so the speed settings changed only lag and
        // overshoot, never the actual distance travelled.
        //
        // HystX/Y holds the previous committed raw sample; capture it
        // before it is overwritten. On a fresh (non-seeded) birth,
        // AmtContactUpdate has already set it to the candidate, so the
        // first sample yields a zero delta and the contact is emitted at
        // its birth position.
        const INT rawDx = (INT)candX - (INT)Contact->HystX;
        const INT rawDy = (INT)candY - (INT)Contact->HystY;

        Contact->HystX = candX;
        Contact->HystY = candY;

        INT stepX, stepY;
        USHORT newReportX, newReportY;

        if (gestureActive) {
            // Scroll frame. FAST swipes get a boost over SLOW/MEDIUM
            // (see FastScaleQ32); both scales apply to the raw delta.
            LONGLONG scaleQ32 = (Velocity == VELOCITY_FAST)
                          ? ScrollRuntime->FastScaleQ32
                          : ScrollRuntime->BaseScaleQ32;

            INT scaledDx = AmtScaleDeltaWithCarry(rawDx, scaleQ32, &Contact->CarryX);
            INT scaledDy = AmtScaleDeltaWithCarry(rawDy, scaleQ32, &Contact->CarryY);

            if (ScrollRuntime->SmoothingAlphaQ32 != AMT_RUNTIME_FIXED_ONE) {
                // Smooth the emitted per-frame step (a velocity filter).
                LONGLONG alphaQ32 = ScrollRuntime->SmoothingAlphaQ32;
                LONGLONG prevQ32 = AMT_RUNTIME_FIXED_ONE - alphaQ32;
                LONGLONG mixX = (LONGLONG)scaledDx * alphaQ32 +
                                (LONGLONG)Contact->ScrollFilteredX * prevQ32;
                LONGLONG mixY = (LONGLONG)scaledDy * alphaQ32 +
                                (LONGLONG)Contact->ScrollFilteredY * prevQ32;
                scaledDx = AmtQ32ProductToIntRound(mixX);
                scaledDy = AmtQ32ProductToIntRound(mixY);
            }
            Contact->ScrollFilteredX = scaledDx;
            Contact->ScrollFilteredY = scaledDy;

            stepX = scaledDx;
            stepY = scaledDy;

            // The sensor-domain position follows the finger exactly while
            // scrolling, so matching and the birth-proximity check keep
            // working in the sensor domain regardless of the scroll scale.
            newReportX = candX;
            newReportY = candY;
        } else {
            // Cursor frame. Retap-seeded first sample always eases in from
            // the seed (no real dt to measure velocity from yet - see
            // AmtContactUpdate). Everything else blends by the continuous
            // alpha the caller computed from measured velocity: heaviest
            // smoothing near-stationary, ramping linearly to a
            // bit-identical raw passthrough by the configured fast velocity.
            INT userAlpha = PointerRuntime->CursorSmoothingAlphaNum;
            INT effAlpha = commitIsRetapSeededFirstSample ? PointerRuntime->CursorSmoothingAlphaNumSlow :
                           ((alphaNum < userAlpha) ? alphaNum : userAlpha);

            // EMA in the sensor domain (against the smoothed sensor
            // position, not the emitted one).
            newReportX = AmtContactSmoothCoord(candX, Contact->ReportX, effAlpha,
                                               PointerRuntime->CursorSmoothingAlphaDen,
                                               PointerRuntime->CursorSmoothingInvAlphaDenQ32);
            newReportY = AmtContactSmoothCoord(candY, Contact->ReportY, effAlpha,
                                               PointerRuntime->CursorSmoothingAlphaDen,
                                               PointerRuntime->CursorSmoothingInvAlphaDenQ32);

            // Cursor speed scales the smoothed sensor-domain step. At 100%
            // this is the identity, so EmitX/Y == ReportX/Y exactly as
            // before the split.
            stepX = AmtScaleDeltaWithCarry((INT)newReportX - (INT)Contact->ReportX,
                                           PointerRuntime->CursorSpeedQ32, &Contact->CarryX);
            stepY = AmtScaleDeltaWithCarry((INT)newReportY - (INT)Contact->ReportY,
                                           PointerRuntime->CursorSpeedQ32, &Contact->CarryY);

            Contact->ScrollFilteredX = 0;
            Contact->ScrollFilteredY = 0;
        }

        Contact->EmitX = AmtClampToRange((LONG)Contact->EmitX + stepX, padMaxX);
        Contact->EmitY = AmtClampToRange((LONG)Contact->EmitY + stepY, padMaxY);
        Contact->ReportX = newReportX;
        Contact->ReportY = newReportY;

        // A tainted contact that moves decisively (non-slow) while it is the
        // only finger down is no longer part of a gesture. (The check used
        // to also read "|| gestureActive", which could never fire here:
        // gestureActive means >=2 fingers, aliveCountIsOne means exactly 1.)
        // Contacts that never move decisively are released by the solo-time
        // expiry in AmtContactUpdate instead. A retap-seeded first sample
        // has no real velocity yet and never clears.
        if (aliveCountIsOne && !commitIsRetapSeededFirstSample && !velocityIsSlow) {
            Contact->WasInGesture = FALSE;
        }
    }

    Contact->PendingFirstSample = FALSE;
    Contact->RetapSeeded        = FALSE; // seed consumed exactly once

    *OutX = Contact->EmitX;
    *OutY = Contact->EmitY;
}

VOID
AmtContactUpdate(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          rawX,
    _In_    USHORT          rawY,
    _In_    USHORT          major,
    _In_    USHORT          minor,
    _In_    USHORT          pressure,
    _In_    USHORT          slotHint,
    _In_    LONGLONG        nowQpc,
    _In_    LONGLONG        PerfFrequencyHz,
    _In_    BOOLEAN         aliveCountIsOne,
    _In_    BOOLEAN         gestureActive,
    _In_    const AMT_POINTER_CONFIG* PointerConfig,
    _In_    const AMT_SCROLL_CONFIG* ScrollConfig,
    _In_    const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_    const AMT_SCROLL_RUNTIME* ScrollRuntime,
    _In_    USHORT          PadMaxX,
    _In_    USHORT          PadMaxY,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
#if DBG
    NT_ASSERT(Contact->State == CONTACT_ACTIVE);
#endif

    BOOLEAN passed;
    BOOLEAN retapSeededFirstSample =
        Contact->PendingFirstSample && Contact->RetapSeeded;

    // LastSeenQpc is still last frame's value here (overwritten below).
    // 0 = no previous sample; handled by the DtQpcTicks<=0 guard.
    LONGLONG prevQpc  = Contact->LastSeenQpc;
    LONGLONG dtTicks  = (prevQpc == 0) ? 0 : (nowQpc - prevQpc);

    // Time basis for velocity: how long the distance-from-anchor (HystX/Y)
    // has been building up, i.e. frames since the anchor was last committed
    // plus this frame - not just this frame (see AMT_VELOCITY_WINDOW_MS).
    // Identical to dtTicks whenever the anchor was committed last frame.
    const LONGLONG windowTicks = (PerfFrequencyHz > 0)
        ? (PerfFrequencyHz * AMT_VELOCITY_WINDOW_MS) / 1000 : 0;
    LONGLONG velocityDtTicks = dtTicks;
    if (dtTicks > 0 && windowTicks > 0) {
        LONGLONG span = dtTicks + (LONGLONG)Contact->HystAgeTicks;
        if (span > windowTicks) {
            // Only the stale part of the anchor's age is bounded. A single
            // frame gap longer than the window (dropped frames, USB stall)
            // is real elapsed time and is never shortened - capping below
            // dtTicks would overstate the speed.
            span = (dtTicks > windowTicks) ? dtTicks : windowTicks;
        }
        velocityDtTicks = span;
    }

    // Scroll gestures classify FAST against their own threshold, decoupled
    // from single-finger cursor movement below. Alpha is only meaningful
    // on the non-gesture path (see AmtContactCommitSample) - it's still
    // The gesture path skips the alpha-ramp work inside AmtContactEvaluateVelocity.
    LONGLONG fastThreshold = gestureActive ? ScrollConfig->FastVelocity
                                            : PointerConfig->CursorFastVelocity;
    LONGLONG slowThreshold = PointerConfig->CursorSlowVelocity;
    LONGLONG alphaFastThreshold = PointerConfig->CursorFastVelocity;
    INT alphaNum;
    CONTACT_VELOCITY_BUCKET velocity = AmtContactEvaluateVelocity(
        rawX, rawY, Contact->HystX, Contact->HystY, velocityDtTicks, PerfFrequencyHz,
        fastThreshold, slowThreshold, alphaFastThreshold,
        PointerRuntime,
        (BOOLEAN)!gestureActive,
        &alphaNum);

    INT deadzoneThreshold = AmtContactDeadzoneForVelocity(velocity, gestureActive, PointerConfig, ScrollConfig);

    // Only the true first sample of a *non*-retap-seeded contact skips the
    // deadzone check (baseline is seeded here, nothing to compare against
    // yet). Every other case - a fresh sample, or a retap-seeded first
    // sample whose HystX/Y baseline already holds the seeded position -
    // runs the same AmtContactEvaluateDeadzone call, so it's kept as one
    // call site instead of being duplicated across two branches.
    if (Contact->PendingFirstSample && !retapSeededFirstSample) {
        Contact->HystX = rawX;
        Contact->HystY = rawY;
        passed = TRUE;
    } else {
        // Keep HystX/Y as-is on the retap-seeded path - they hold the
        // seeded baseline.
        passed = AmtContactEvaluateDeadzone(Contact, rawX, rawY, deadzoneThreshold);
    }

    AmtContactCommitSample(Contact, rawX, rawY, passed, aliveCountIsOne,
                           gestureActive, velocity,
                           alphaNum, retapSeededFirstSample,
                           PointerRuntime, ScrollRuntime,
                           PadMaxX, PadMaxY,
                           OutX, OutY);

    // Anchor age: reset when the anchor moved (this includes a birth, where
    // it is seeded), otherwise it keeps growing, bounded by the window.
    if (passed) {
        Contact->HystAgeTicks = 0;
    } else if (dtTicks > 0 && windowTicks > 0) {
        LONGLONG age = (LONGLONG)Contact->HystAgeTicks + dtTicks;
        if (age > windowTicks)
            age = windowTicks;
        Contact->HystAgeTicks = (ULONG)age;
    }

    // Gesture-taint expiry: a contact that has been the only finger down
    // for AMT_GESTURE_TAINT_EXPIRY_MS is no longer part of a gesture, even
    // if it never moved decisively. Any >=2-finger frame restarts the clock.
    if (aliveCountIsOne) {
        if (dtTicks > 0 && PerfFrequencyHz > 0) {
            LONGLONG addMs = (dtTicks * 1000 + PerfFrequencyHz / 2) / PerfFrequencyHz;
            LONGLONG ms = (LONGLONG)Contact->SoloMs + addMs;
            Contact->SoloMs = (USHORT)(ms > 0xFFFF ? 0xFFFF : ms);
        }
        if (Contact->WasInGesture && Contact->SoloMs >= AMT_GESTURE_TAINT_EXPIRY_MS)
            Contact->WasInGesture = FALSE;
    } else {
        Contact->SoloMs = 0;
    }

    Contact->LastSlotHint = slotHint;
    Contact->LastSeenQpc  = nowQpc;
    Contact->LastMajor    = major;
    Contact->LastMinor    = minor;
    Contact->LastPressure = pressure;

    if (Contact->FramesAlive < 255)
        Contact->FramesAlive++;
}

#if DBG
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        const ACTIVE_CONTACT* c = &Pool[i];

        if (c->State == CONTACT_FREE) {
            NT_ASSERT(!c->PendingFirstSample);
            NT_ASSERT(!c->RetapSeeded);
            NT_ASSERT(!c->WasInGesture);
            NT_ASSERT(c->ContactID == 0);
            continue;
        }

        NT_ASSERT(c->ContactID != 0);

        for (size_t j = i + 1; j < MAX_CONTACTS; j++) {
            const ACTIVE_CONTACT* d = &Pool[j];
            if (d->State == CONTACT_FREE) continue;
            NT_ASSERT(c->ContactID != d->ContactID);
        }
    }
}
#endif