// Contract between the input and report layers.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

// Raw input from the adapter.

typedef struct _RAW_CONTACT
{
    USHORT SlotIndex;   // scan-order position in this frame's finger array
                        // (0..PTP_MAX_CONTACT_POINTS-1) - NOT a stable
                        // hardware identity, the firmware doesn't expose
                        // one. Matching hint only; see Match.c.
    USHORT X;            // normalized device units (post AmtInputClampCoord)
    USHORT Y;
    SHORT  RawX;         // exact raw USB abs_x
    SHORT  RawY;         // exact raw USB abs_y
    USHORT Major;        // touch_major, raw
    USHORT Minor;        // touch_minor, raw
    USHORT Orientation;  // raw Apple orientation (signed 15-bit angle; 16384 = point)
    USHORT Pressure;     // force-touch trackpad pressure, raw ADC units (~0-300)
    UCHAR  Origin;        // firmware origin field; 0 == identity break signal
} RAW_CONTACT, *PRAW_CONTACT;

typedef struct _RAW_FRAME
{
    LONGLONG    TimestampQpc;
    UCHAR       ContactCount;
    RAW_CONTACT Contacts[PTP_MAX_CONTACT_POINTS];
} RAW_FRAME, *PRAW_FRAME;

// Stable contact identities and FSM phase.

typedef enum _CONTACT_PHASE
{
    CONTACT_PHASE_NONE = 0,  // no contact this frame
    CONTACT_PHASE_DOWN,      // born this frame (FREE -> ACTIVE)
    CONTACT_PHASE_MOVE,      // continuing (ACTIVE, updated)
    CONTACT_PHASE_UP,        // lifted this frame (-> FREE or GRACE)
} CONTACT_PHASE;

// Output contract for the interrupt layer.
typedef struct _PTP_CORE_CONTACT
{
    ULONG          ContactID;     // stable, monotonic, never reused while warm
    USHORT         X;
    USHORT         Y;
    CONTACT_PHASE  Phase;
    BOOLEAN        Confident;     // FALSE only for brand-new, unverified
                                   // contacts (Match.c Unconfirmed).
    BOOLEAN        PalmSuspect;   // local-palm classification; suppressed
                                  // contacts don't appear in PTP_CORE_FRAME
} PTP_CORE_CONTACT, *PPTP_CORE_CONTACT;

typedef struct _PTP_CORE_FRAME
{
    LONGLONG          TimestampQpc;
    UCHAR             ContactCount;
    PTP_CORE_CONTACT  Contacts[PTP_MAX_CONTACT_POINTS];
    BOOLEAN           LargePalmBlanked;  // whole-pad palm event this frame
} PTP_CORE_FRAME, *PPTP_CORE_FRAME;

// Process one frame of touch and button state.

struct _DEVICE_CONTEXT; // fwd decl, defined in Device.h

// OutForceTouchClick reports a synthetic right-click once per qualifying press.
// OutButtonClickReport carries the final ordinary-click decision.
VOID
PTPCore_ProcessFrame(
    _Inout_ struct _DEVICE_CONTEXT* DeviceContext,
    _In_    const RAW_FRAME*        RawFrame,
    _In_    LONGLONG                NowQpc,
    _In_    BOOLEAN                 ButtonDown,
    _Out_   PTP_CORE_FRAME*         OutResult,
    _Out_   BOOLEAN*                OutForceTouchClick,
    _Out_   BOOLEAN*                OutButtonClickReport
);

// Recent-lift ring buffer for retap smoothing.

#define RECENT_LIFT_CAPACITY PTP_MAX_CONTACT_POINTS

// Safety cap prevents a click-arbitration press from staying pending
// indefinitely. Shared with Device.c (D0Entry tick-cache) and Ptpcore.c.
#define CLICK_ARBITRATION_TIMEOUT_MS 90

// MICRO-OPT: fields ordered widest-first (LONGLONG before the USHORTs
// before the BOOLEAN). Read via field names only everywhere
// (Ptpcore.c/Activecontact.c), no layout-dependent code.
//
// Two positions are kept per lift because scroll scale / cursor speed make
// the Windows-facing position drift away from the sensor position:
//   X/Y          sensor domain (smoothed raw) - proximity test against raw
//                candidates in AmtRecentLiftFindNearby.
//   EmitX/EmitY  where Windows last saw the contact - seeds the re-tapped
//                contact so a quick second tap lands on the same reported
//                spot.
typedef struct _RECENT_LIFT
{
    LONGLONG LiftQpc;
    USHORT   X;
    USHORT   Y;
    USHORT   EmitX;
    USHORT   EmitY;
    BOOLEAN  Valid;
} RECENT_LIFT;

typedef struct _RECENT_LIFT_RING
{
    RECENT_LIFT Entries[RECENT_LIFT_CAPACITY];
    UCHAR       NextWriteIndex; // round-robin
} RECENT_LIFT_RING;

VOID
AmtRecentLiftRecord(
    _Inout_ RECENT_LIFT_RING* Ring,
    _In_    LONGLONG          NowQpc,
    _In_    USHORT            X,
    _In_    USHORT            Y,
    _In_    USHORT            EmitX,
    _In_    USHORT            EmitY
);

// Find closest lift to (CandX, CandY). FALSE -> raw birth.
// WindowTicks: precomputed (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000,
// cached once in DEVICE_CONTEXT at D0Entry - MICRO-OPT, avoids recomputing
// this 64-bit multiply+divide on every candidate birth. 0 means "no usable
// clock", same fail-closed behavior as the old PerfFrequencyHz<=0 check.
BOOLEAN
AmtRecentLiftFindNearby(
    _In_  const RECENT_LIFT_RING* Ring,
    _In_  LONGLONG                NowQpc,
    _In_  LONGLONG                WindowTicks,
    _In_  USHORT                  CandX,
    _In_  USHORT                  CandY,
    _Out_ USHORT*                 OutX,      // lift, sensor domain
    _Out_ USHORT*                 OutY,
    _Out_ USHORT*                 OutEmitX,  // lift, Windows-facing
    _Out_ USHORT*                 OutEmitY
);

EXTERN_C_END