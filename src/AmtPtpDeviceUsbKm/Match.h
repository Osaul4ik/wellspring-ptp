// Match.h - Cost-based matcher between RawFrame contacts and ACTIVE_CONTACT pool.
// Slot index is a matching hint only, never direct index.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"
#include "Palm.h"

EXTERN_C_START

// Candidate contact produced from the current frame.
typedef struct _MATCH_CANDIDATE
{
    USHORT  SlotIndex;      // hw slot - HINT only
    USHORT  X;
    USHORT  Y;
    USHORT  Major;          // touch_major, raw - matching tie-break only
    USHORT  Minor;          // touch_minor, raw - matching tie-break only
    USHORT  Pressure;       // raw ADC units - matching tie-break only
    BOOLEAN PalmLocal;       // excluded from matching/reporting
    BOOLEAN IdentityBreak;   // firmware origin==0 signal
    UCHAR   TipDropApplied;  // non-zero when X/Y is stale (debounce bridge).
                             // Position provenance only - does NOT drive
                             // Confidence.

    // TRUE for a brand-new, below-tip-threshold contact with no pool anchor.
    // Matching/gesture-taint quality signal ONLY (aliveCount gating in
    // Ptpcore.c) - does NOT drive Confidence. Confidence is decided purely
    // by AmtPalmClassify (Palm.c); see the reportConfident comment in
    // Ptpcore.c for the full rationale.
    BOOLEAN Unconfirmed;
} MATCH_CANDIDATE;

typedef struct _MATCH_CANDIDATE_SET
{
    UCHAR           Count;
    MATCH_CANDIDATE Candidates[PTP_MAX_CONTACT_POINTS];
} MATCH_CANDIDATE_SET;

// Correspondence result: pool index or MATCH_NO_CORRESPONDENCE.
#define MATCH_NO_CORRESPONDENCE  ((size_t)-1)

typedef struct _MATCH_RESULT
{
    // MICRO-OPT: both size_t[] arrays grouped first (8-byte aligned,
    // contiguous, zero gap between them) so the BOOLEAN/UCHAR tail only
    // pays for trailing padding once instead of twice - was 96 bytes
    // (10 bytes padding), now 88 (2 bytes). Field meaning/order-by-name
    // unchanged, purely internal layout.

    // Parallel to Candidates[]. Pool index or MATCH_NO_CORRESPONDENCE.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // Pool indices with no corresponding candidate (should lift).
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];

    // Identity broken (lift + birth) despite correspondence.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// Build the candidate set for matching.
VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_  const AMT_PALM_CONFIG*                   PalmConfig,
    _In_  const AMT_PALM_RUNTIME*                  PalmRuntime,
    _In_  const AMT_POINTER_CONFIG*                PointerConfig,
    _In_  BOOLEAN                                   SupportsForceTouch,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
);

// Max per-frame finger movement for continuation/bridging, in normalized
// sensor units. Deliberately generous (several fast frames' worth, and it
// also bounds how far a frozen edge-zone contact may wander before it is
// no longer associated with its palm-local candidate). Shared by Match.c
// (correspondence, tip-drop bridging) and Ptpcore.c (edge-zone freeze).
#define MATCH_MAX_CONTINUATION_DELTA 4000

// "Is a finger already tracked right here?" radius for the geometric
// birth test that feeds the edge-zone hard reject in AmtPalmClassify.
// Separate from (and much smaller than) the continuation delta: that one
// spans ~30-40% of the pad width, which made almost any new touch look
// like a continuation of some other finger and so silently bypassed the
// edge hard reject whenever a finger was already down. This only needs to
// cover the real per-frame travel of one finger plus a few dropped
// frames (~1.5 cm at ~1000 units/cm on current models). Engineering
// estimate - validate with the live monitor and adjust if needed.
#define MATCH_BIRTH_PROXIMITY_DELTA 1500

// Max time gap for same-ContactID continuation (150ms).
#define MATCH_MAX_TIME_DELTA_100NS (150LL * 10000LL)

// Match contacts greedily and reject bad or stale pairs.
// MaxTicks: precomputed (MATCH_MAX_TIME_DELTA_100NS * PerfFrequencyHz) / 10000000,
// cached once in DEVICE_CONTEXT at D0Entry - MICRO-OPT, avoids recomputing
// this 64-bit multiply+divide on every accepted match. 0 means "no usable
// clock", same fail-closed behavior as the old PerfFrequencyHz<=0 check.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  MaxTicks,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END