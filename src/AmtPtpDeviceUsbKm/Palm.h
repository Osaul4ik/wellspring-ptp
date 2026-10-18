// Palm classification for a single sample.

#pragma once

#include "PTPCore.h"

EXTERN_C_START

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

// Once a large palm has been seen, PTPCore keeps suppressing the pad (the
// palm's remains look like fingers as its shape breaks up) for as long as
// any contact is still near where the palm was; the anchor follows the
// nearest contact frame to frame. When nothing is left within this radius
// (sensor units, ~a quarter to a third of the pad width) the palm is gone
// and fingers elsewhere resume - instead of staying frozen until EVERY
// contact, however far away, has lifted. Engineering estimate; validate
// with real palm captures.
#define PALM_LATCH_RELEASE_RADIUS 3000

typedef struct _AMT_PALM_RUNTIME
{
    LONGLONG EdgeFactorTopQ32;
    LONGLONG EdgeFactorLeftQ32;
    LONGLONG EdgeFactorRightQ32;
    LONGLONG EdgeFactorBottomQ32;
} AMT_PALM_RUNTIME, *PAMT_PALM_RUNTIME;

// Classify one contact as palm or finger.
// IsBirth: TRUE when this raw contact is a fresh finger per firmware
// (Origin==0 / identity-break signal), not a continuation of a contact
// already being tracked. A birth landing inside the edge zone is a hard
// PALM_LOCAL reject regardless of shape - continuations that merely pass
// through the zone are still judged by the shape/score heuristic below.
//
// Config: runtime-tunable thresholds (see AMT_PALM_CONFIG in Public.h).
// Never NULL - callers pass &DeviceContext->PalmConfig, which is always
// initialized (to AMT_PALM_CONFIG_DEFAULT_INIT, then optionally overridden
// from the registry/GUI) before the first frame is ever processed.
PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ const AMT_PALM_CONFIG*       Config,
    _In_ const AMT_PALM_RUNTIME*      Runtime,
    _In_ INT                          NormX,
    _In_ INT                          NormY,
    _In_ BOOLEAN                      IsBirth,
    _In_ BOOLEAN                      GestureContext
);

// TRUE if (Major, Minor) is finger-shaped (shape heuristic only, ignoring
// the edge zone): i.e. would classify as PALM_NONE away from the edge.
BOOLEAN
AmtPalmLooksLikeFinger(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ const AMT_PALM_CONFIG*       Config
);

// TRUE if the normalized position lies inside the configured edge zone.
BOOLEAN
AmtPalmInEdgeZone(
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ const AMT_PALM_RUNTIME*      Runtime,
    _In_ INT                          NormX,
    _In_ INT                          NormY
);

EXTERN_C_END