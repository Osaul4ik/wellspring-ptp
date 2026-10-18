// Contact lifecycle and pool state for active touch tracking.
// Pool slots are separate from hardware slots.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

// Precomputed fixed-point coefficients used only by the input hot path.
// User-facing percentages remain in AMT_*_CONFIG for IOCTL/registry compatibility,
// but the interrupt path never performs percent division.
#define AMT_RUNTIME_FIXED_SHIFT 32
#define AMT_RUNTIME_FIXED_ONE   (1LL << AMT_RUNTIME_FIXED_SHIFT)

typedef struct _AMT_POINTER_RUNTIME
{
    LONGLONG CursorSpeedQ32;
    INT  CursorSmoothingAlphaNum;
    INT  CursorSmoothingAlphaNumSlow;
    INT  CursorSmoothingAlphaDen;
    LONGLONG CursorSmoothingSlopeQ32;

    // Reciprocal of CursorSmoothingAlphaDen, Q32 (1/AlphaDen). Lets the
    // hot-path blend in AmtContactSmoothCoord replace a per-contact,
    // per-frame runtime division by AlphaDen with a multiply+shift,
    // consistent with "the interrupt path never performs percent
    // division" above - AlphaDen is a config-runtime value (changes only
    // on SET_POINTER_CONFIG), so its reciprocal only needs recomputing
    // here, not every frame.
    LONGLONG CursorSmoothingInvAlphaDenQ32;
} AMT_POINTER_RUNTIME, *PAMT_POINTER_RUNTIME;

typedef struct _AMT_SCROLL_RUNTIME
{
    LONGLONG BaseScaleQ32;
    LONGLONG FastScaleQ32;
    LONGLONG SmoothingAlphaQ32;
} AMT_SCROLL_RUNTIME, *PAMT_SCROLL_RUNTIME;

typedef enum _CONTACT_STATE
{
    CONTACT_FREE = 0,  // free pool slot, no identity
    CONTACT_ACTIVE,    // finger down, identity live
    CONTACT_GRACE,     // same-frame quarantine after gesture lift
} CONTACT_STATE;

// One pool entry; pool slot is not the identity.
//
// MICRO-OPT: aligned/padded to 64 bytes (Intel L1/L2/L3 cache line size,
// including i9-9750H) so each pool entry occupies exactly one cache line
// instead of straddling two (natural size was 48B, so entries used to
// drift across line boundaries in the 5-entry array).
//
// CAVEAT: this is a compiler-side alignment hint, not a runtime guarantee.
// ACTIVE_CONTACT lives inside DEVICE_CONTEXT, which WDF allocates via its
// own pool allocator (guarantees only MEMORY_ALLOCATION_ALIGNMENT = 16B on
// x64, not 64B). So the array may or may not actually land on 64B-aligned
// addresses at runtime - the compiler emits code assuming it does, but
// nothing enforces it. Harmless either way (x64 tolerates unaligned
// ordinary loads/stores - no crash risk), but treat this as "free if it
// works, no-op if it doesn't", not a guaranteed win.
typedef struct DECLSPEC_ALIGN(64) _ACTIVE_CONTACT
{
    CONTACT_STATE State;

    // Windows-facing identity. Valid while State != CONTACT_FREE.
    // Never reused while warm.
    ULONG ContactID;

    // Smoothed SENSOR-domain position (post deadzone + EMA of the raw
    // position), normalized device units. This is the matching/proximity
    // domain: Match.c, the retap ring and the birth-proximity check all
    // compare it against raw candidate coordinates, so it must always stay
    // within smoothing-lag distance of where the finger physically is.
    // It is NOT what Windows sees - see EmitX/Y below.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline: the last raw position that passed the
    // deadzone (sensor domain). Distinct from ReportX/Y (post-EMA).
    USHORT HystX;
    USHORT HystY;

    // Windows-facing position: what the PTP report actually carries. It is
    // integrated from SCALED sensor-domain deltas (scroll scale / cursor
    // speed), so it intentionally drifts away from ReportX/Y - scaling an
    // absolute position can only be done by scaling its delta. Never used
    // for matching.
    USHORT EmitX;
    USHORT EmitY;

    // TRUE if this contact took part in a >=2-finger frame. It is what
    // routes the contact's lift through the gesture path (no RecentLifts
    // entry, so no retap smoothing off a gesture lift) and what lets the
    // tail-overlap logic in PTPCore tell a dying gesture's straggler from a
    // genuine new gesture start. It is NOT permanent: it clears when the
    // contact moves decisively on its own (a non-slow solo frame) or once it
    // has been the only finger down for AMT_GESTURE_TAINT_EXPIRY_MS
    // (SoloMs below) - without the expiry a straggler that stayed put
    // would stay "in a gesture" until it lifted.
    BOOLEAN WasInGesture;

    // First-sample flag; skips the initial deadzone/EMA path when needed.
    BOOLEAN PendingFirstSample;

    // Keeps the retap seed from being overwritten on the first sample.
    BOOLEAN RetapSeeded;

    // Fractional remainder (Q32, always in [0, 2^32)) of the scaled delta
    // that has not yet been emitted, carried across frames so scaling
    // never loses sub-unit motion (e.g. 0.36x scroll at slow speed).
    ULONG CarryX;
    ULONG CarryY;
    LONG ScrollFilteredX;
    LONG ScrollFilteredY;

    // ---- Matching-hint fields. NOT identity. ----
    USHORT   LastSlotHint;    // hw slot matched to last frame; speeds up matching

    // Milliseconds this contact has been the ONLY finger down (saturating);
    // reset whenever a gesture frame (>=2 fingers) is seen. Drives the
    // WasInGesture expiry.
    USHORT   SoloMs;

    // Ticks since HystX/Y was last committed, capped at the velocity window
    // (AMT_VELOCITY_WINDOW_MS). Velocity is distance-from-HystX/Y over the
    // time that distance took to build up - see AmtContactUpdate.
    ULONG    HystAgeTicks;

    LONGLONG LastSeenQpc;     // QPC of last successful match; grace/retap timing

    // Geometry/pressure, used ONLY as a matching tie-break (Match.c).
    // 0 until the first AmtContactUpdate call.
    USHORT   LastMajor;
    USHORT   LastMinor;
    USHORT   LastPressure;

    // Frames this contact has been alive, saturating at 255. NOT an
    // identity/matching hint - purely a diagnostic/age counter.
    UCHAR FramesAlive;

    // TRUE once a TipSwitch=1 report carrying the CURRENT ContactID has
    // actually been placed in a frame sent to Windows (not merely queued in
    // the overflow queue, which can drop a stale DOWN). Windows must never
    // receive an UP for an ID it never saw go down, so lift paths consult
    // this. Cleared on birth and on identity rebind.
    BOOLEAN Announced;
} ACTIVE_CONTACT, *PACTIVE_CONTACT;

// Keep each pool element exactly one 64-byte cache line.
// Velocity is measured as distance from the deadzone anchor (HystX/Y) over
// the time since that anchor was committed - not over one frame. While the
// deadzone holds the anchor still across several frames the distance
// accumulates over all of them, so dividing it by a single frame's dt
// inflated the speed by the number of held frames and made slow drift look
// like MEDIUM/FAST (defeating the slow deadzone and slow smoothing). The
// window bounds the averaging so a finger that starts moving after resting
// is not judged against the whole rest: past this age the anchor is
// "stale" and the speed is taken over the window instead.
#define AMT_VELOCITY_WINDOW_MS 50

// A contact that has been the only finger down this long is no longer part
// of a gesture, whatever its speed (~12 frames at 120 Hz; comfortably longer
// than the one-to-few-frame tail overlap the taint logic exists for).
#define AMT_GESTURE_TAINT_EXPIRY_MS 100

// Multi-finger landing settle window. Windows decides which gesture a touch
// is (cursor / 2-finger scroll / 3- or 4-finger swipe) from the number of
// contacts present when motion starts. Fingers rarely land in the same
// frame: when the first finger is already moving and the others follow one
// by one, motion emitted while the contact set is still growing makes the
// recogniser lock onto the wrong gesture (scroll instead of a 4-finger
// swipe, 3 fingers instead of 4). After a finger lands next to at least one
// other contact, emitted motion is deferred for this long; the deferred
// distance is released in one step by the normal deadzone path afterwards,
// so nothing is lost and the contacts' geometry stays consistent. Each
// further landing restarts the window. 0 disables the hold.
#define AMT_GESTURE_SETTLE_MS 40

C_ASSERT(sizeof(ACTIVE_CONTACT) == 64);

#define MAX_CONTACTS PTP_MAX_CONTACT_POINTS  // pool capacity, not slot count

// Zero/FREE-initialise the whole pool. Call at device creation and D0Entry.
VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool);

// Finds a FREE pool entry. Returns index or MAX_CONTACTS if full.
size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);

// FREE -> ACTIVE. Assigns fresh ContactID, seeds baseline.
// Precondition: Pool[index].State == CONTACT_FREE.
VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
);

// Rebind an active contact to a new identity without resetting smoothing state.
VOID
AmtContactRebindIdentity(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _Out_   ULONG*          OldContactID
);

// Birth a contact with a retap-based seed. RecentLiftX/Y is the previous
// lift in the sensor (matching) domain and seeds the smoothing/deadzone
// baselines; RecentLiftEmitX/Y is where Windows last saw that contact and
// seeds the emitted position, so a quick re-tap lands on the same
// reported spot even when scaling had moved EmitX/Y away from ReportX/Y.
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
);

// Recent-lift window/distance thresholds. Consumed by the actual
// slot-independent retap check - AmtRecentLiftFindNearby in PTPCore.c/.h,
// which operates over the RECENT_LIFT_RING instead of a single lift sample.
#define RETAP_WINDOW_100NS      (150LL * 10000LL)  // was 700 ms
#define RETAP_MAX_DISTANCE      200                 // was 600 (normalized units)

// Look up the sensor-domain position (ReportX/Y - where the finger
// physically is, NOT what Windows was told) of the live contact that carries
// ContactID. Returns FALSE (outputs untouched) when no such contact is in the
// pool - e.g. a contact that was lifted this frame. Read-only; used by the
// live monitor so it can pair emitted contacts with raw sensor data and draw
// the real finger position even while scroll scale / cursor speed make the
// emitted position drift.
BOOLEAN
AmtContactPoolFindSensorPos(
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _In_  ULONG   ContactID,
    _Out_ USHORT* SensorX,
    _Out_ USHORT* SensorY
);

// Lift a contact and return its last state. OldX/OldY is the last position
// Windows saw (EmitX/Y), i.e. the right place for the UP report. Callers
// that need the sensor-domain position (retap bookkeeping) must read
// ReportX/Y BEFORE calling - the slot is zeroed here.
VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// Move an active contact into grace state for one frame.
VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// Drop a grace contact and clear its slot.
VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index);

// Check whether a candidate passes the deadzone test.
BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY,
    _In_ INT                   ThresholdUnits
);

// Update an active contact for one frame using deadzone and EMA.
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
    _In_    BOOLEAN         gestureFast,   // frame-level FAST decision (AmtContactGestureIsFast); scroll scale is
                                           // chosen once per frame so every finger of a gesture scales alike
    _In_    BOOLEAN         holdMotion,    // landing-settle hold (AMT_GESTURE_SETTLE_MS): defer emitted motion
    _In_    const AMT_POINTER_CONFIG* PointerConfig,
    _In_    const AMT_SCROLL_CONFIG* ScrollConfig,
    _In_    const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_    const AMT_SCROLL_RUNTIME* ScrollRuntime,
    _In_    USHORT          PadMaxX,   // normalized pad extent: 0..PadMax
    _In_    USHORT          PadMaxY,
    _Out_   USHORT*         OutX,      // Windows-facing (EmitX/Y), clamped to the pad
    _Out_   USHORT*         OutY
);

// Would this (non-newborn) contact classify as FAST on a scroll/gesture
// frame if updated with (rawX, rawY) now? Same velocity maths and thresholds
// as AmtContactUpdate uses on the gesture path, evaluated read-only so the
// caller can take ONE frame-level decision across all gesture contacts
// before any of them is updated. FALSE when there is no timing basis yet.
BOOLEAN
AmtContactGestureIsFast(
    _In_ const ACTIVE_CONTACT*      Contact,
    _In_ USHORT                     rawX,
    _In_ USHORT                     rawY,
    _In_ LONGLONG                   nowQpc,
    _In_ LONGLONG                   PerfFrequencyHz,
    _In_ const AMT_POINTER_CONFIG*  PointerConfig,
    _In_ const AMT_SCROLL_CONFIG*   ScrollConfig,
    _In_ const AMT_POINTER_RUNTIME* PointerRuntime
);

#if DBG
// Debug invariants.
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);
#else
#define AmtContactPoolCheckInvariants(Pool) ((VOID)0)
#endif

EXTERN_C_END