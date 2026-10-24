// ConfigIoctl.c - AmtPtpConfigGui <-> driver custom IOCTL surface.
//
// This is the kernel side of the "Elan-style" palm-rejection control panel.
// Every entry point here is reachable from user mode via
// IOCTL_AMT_PTP_{GET,SET}_PALM_CONFIG / GET_PAD_GEOMETRY / RESET_PALM_CONFIG
// (Queue.c dispatches EvtIoDeviceControl - the EXTERNAL IOCTL path - to
// these functions; see Queue.c for the dispatch switch). All of them are
// METHOD_BUFFERED, so WdfRequestRetrieveInputBuffer/OutputBuffer already
// give us a kernel-side copy - no direct user pointers touched here.

#include "Driver.h"
#include "ConfigIoctl.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPalmConfigLoadFromRegistry)
#pragma alloc_text (PAGE, AmtPointerConfigLoadFromRegistry)
#pragma alloc_text (PAGE, AmtScrollConfigLoadFromRegistry)
#endif

// ----------------------------------------------------------------------------
// Clamp helper - shared by the SET IOCTL and the registry loader, so a
// corrupt registry value can never do anything worse than "silently
// clamped back into range", never a driver bugcheck or a degenerate
// classifier state (e.g. an edge zone that swallows the whole pad, or a
// PalmMinMajor above PalmLargeMajor).
// ----------------------------------------------------------------------------

static ULONG
AmtClampULong(_In_ ULONG Value, _In_ ULONG Min, _In_ ULONG Max)
{
    if (Value < Min) return Min;
    if (Value > Max) return Max;
    return Value;
}

// Clamp, then re-quantize onto a Step grid (round to nearest, not
// truncate), then clamp again - the second clamp only matters when
// rounding pushes an edge value one step past Min/Max. Shared by every
// GUI-slider-backed field that must land on the same grid the slider
// itself steps in, so a hand-crafted IOCTL or a hand-edited registry
// value can't land between steps. Replaces three near-identical
// clamp/round/clamp blocks below (hold-ms, and the two drag-lockout
// distances).
static ULONG
AmtClampToStepGrid(_In_ ULONG Value, _In_ ULONG Min, _In_ ULONG Max, _In_ ULONG Step)
{
    Value = AmtClampULong(Value, Min, Max);
    Value = ((Value + (Step / 2)) / Step) * Step;
    return AmtClampULong(Value, Min, Max);
}

static LONGLONG
AmtPercentToQ32(_In_ ULONG Percent)
{
    return (((LONGLONG)Percent * AMT_RUNTIME_FIXED_ONE) + 50) / 100;
}

static LONGLONG
AmtRatioToQ32(_In_ ULONG Numerator, _In_ ULONG Denominator)
{
    LONGLONG scaled;

    if (Denominator == 0)
        return 0;

    scaled = ((LONGLONG)Numerator * AMT_RUNTIME_FIXED_ONE +
              (Denominator / 2)) / Denominator;

    return scaled;
}

VOID
AmtPointerRuntimeRebuild(
    _In_ const AMT_POINTER_CONFIG* Config,
    _Out_ AMT_POINTER_RUNTIME* Runtime
)
{
    ULONG alphaNum;

    Runtime->CursorSpeedQ32 = AmtPercentToQ32(Config->CursorSpeedPercent);
    Runtime->CursorSmoothingAlphaNumSlow = (INT)Config->SmoothingAlphaNumSlow;
    Runtime->CursorSmoothingAlphaDen = (INT)Config->SmoothingAlphaDen;

    // Reciprocal of AlphaDen, Q32 - lets AmtContactSmoothCoord (hot path,
    // per contact/per coordinate/every frame) replace a runtime division
    // by AlphaDen with a multiply+shift. AlphaDen only changes here (on
    // SET_POINTER_CONFIG), never per-frame, so this is the right place
    // to pay the one division needed to produce the reciprocal.
    Runtime->CursorSmoothingInvAlphaDenQ32 =
        AmtRatioToQ32(1, Config->SmoothingAlphaDen);

    {
        ULONG span = Config->SmoothingAlphaDen - Config->SmoothingAlphaNumSlow;
        Runtime->CursorSmoothingSlopeQ32 =
            (span == 0) ? 0 : AmtRatioToQ32(span,
                                            Config->CursorFastVelocity - Config->CursorSlowVelocity);
    }

    if (Config->CursorSmoothingPercent == 0) {
        alphaNum = Config->SmoothingAlphaDen;
    } else {
        ULONG span = Config->SmoothingAlphaDen - Config->SmoothingAlphaNumSlow;
        alphaNum = Config->SmoothingAlphaNumSlow +
                   (ULONG)((((LONGLONG)span * Config->CursorSmoothingPercent) + 99) / 100);
        if (alphaNum > Config->SmoothingAlphaDen)
            alphaNum = Config->SmoothingAlphaDen;
    }

    Runtime->CursorSmoothingAlphaNum = (INT)alphaNum;
}

VOID
AmtPointerForceTouchTimingRebuild(_Inout_ PDEVICE_CONTEXT DeviceContext)
{
    // Same ms->ticks conversion D0Entry already does once for
    // ClickArbitrationTimeoutTicks (see that field's comment in Device.h) -
    // this is the equivalent for the user-tunable emulation hold duration,
    // which additionally needs recomputing here on every config change
    // (ClickArbitrationTimeoutTicks doesn't, since CLICK_ARBITRATION_TIMEOUT_MS
    // is a compile-time constant).
    if (DeviceContext->PerfFrequency.QuadPart > 0) {
        DeviceContext->ForceTouchEmulationHoldTicks =
            (DeviceContext->PerfFrequency.QuadPart *
             (LONGLONG)DeviceContext->PointerConfig.ForceTouchEmulationHoldMs) / 1000;
    } else {
        DeviceContext->ForceTouchEmulationHoldTicks = 0;
    }

    // No conversion needed for the two distances - just moving them out of
    // PointerConfig so PTPCore_ProcessFrame's hot path has a single,
    // path-agnostic field to read (see Device.h field comments).
    DeviceContext->ForceTapDragLockoutDistanceCached =
        (USHORT)DeviceContext->PointerConfig.ForceTapDragLockoutDistance;
    DeviceContext->ForceTouchEmulationDragLockoutDistanceCached =
        (USHORT)DeviceContext->PointerConfig.ForceTouchEmulationDragLockoutDistance;
}

VOID
AmtScrollRuntimeRebuild(
    _In_ const AMT_SCROLL_CONFIG* Config,
    _Out_ AMT_SCROLL_RUNTIME* Runtime
)
{
    Runtime->BaseScaleQ32 = AmtRatioToQ32(
        Config->ScaleNum * Config->SpeedPercent,
        Config->ScaleDen * 100);
    Runtime->FastScaleQ32 = AmtRatioToQ32(
        Config->ScaleNumFast * Config->FastSpeedPercent,
        Config->ScaleDenFast * 100);

    {
        ULONG alphaPercent = 100 - Config->SmoothingPercent;
        if (alphaPercent < 10)
            alphaPercent = 10;
        Runtime->SmoothingAlphaQ32 = AmtPercentToQ32(alphaPercent);
    }
}

VOID
AmtPalmRuntimeRebuild(
    _In_ const AMT_PALM_CONFIG* Config,
    _Out_ AMT_PALM_RUNTIME* Runtime
)
{
    // Palm edge permille -> Q32 factor is computed only when configuration
    // changes; the per-contact hot path performs multiply+shift only.
    Runtime->EdgeFactorTopQ32 = AmtRatioToQ32(Config->EdgePermilleTop, 1000);
    Runtime->EdgeFactorLeftQ32 = AmtRatioToQ32(Config->EdgePermilleLeft, 1000);
    Runtime->EdgeFactorRightQ32 = AmtRatioToQ32(Config->EdgePermilleRight, 1000);
    Runtime->EdgeFactorBottomQ32 = AmtRatioToQ32(Config->EdgePermilleBottom, 1000);
}

VOID
AmtPalmConfigClamp(_Inout_ PAMT_PALM_CONFIG Config)
{
    Config->StructVersion = AMT_PALM_CONFIG_VERSION;

    Config->EdgePermilleTop    = AmtClampULong(Config->EdgePermilleTop,    0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleLeft   = AmtClampULong(Config->EdgePermilleLeft,   0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleRight  = AmtClampULong(Config->EdgePermilleRight, 0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleBottom = AmtClampULong(Config->EdgePermilleBottom,0, AMT_PALM_EDGE_PERMILLE_MAX);

    Config->PalmLargeMajor  = AmtClampULong(Config->PalmLargeMajor,  1, AMT_PALM_MAJOR_MAX);
    Config->PalmLargeRatio  = AmtClampULong(Config->PalmLargeRatio,  1, AMT_PALM_RATIO_MAX);
    Config->PalmScoreThresh = AmtClampULong(Config->PalmScoreThresh,1, AMT_PALM_SCORE_MAX);
    Config->PalmMinMajor    = AmtClampULong(Config->PalmMinMajor,   0, AMT_PALM_MAJOR_MAX);
    Config->PalmMinMinor    = AmtClampULong(Config->PalmMinMinor,   0, AMT_PALM_MAJOR_MAX);

    Config->PalmLockReleaseWhenGone = Config->PalmLockReleaseWhenGone ? 1 : 0;

    RtlZeroMemory(Config->Reserved, sizeof(Config->Reserved));
}

VOID
AmtPointerConfigClamp(_Inout_ PAMT_POINTER_CONFIG Config)
{
    Config->StructVersion = AMT_POINTER_CONFIG_VERSION;
    Config->ForceTapThreshold = AmtClampULong(Config->ForceTapThreshold, AMT_POINTER_THRESHOLD_MIN, AMT_POINTER_THRESHOLD_MAX);
    if (Config->ForceTapAction > AMT_POINTER_ACTION_MAX)
        Config->ForceTapAction = AMT_POINTER_ACTION_CONTEXT_MENU;
    Config->ForceTouchEnabled = Config->ForceTouchEnabled ? 1u : 0u;
    Config->RequirePressureToActivate = Config->RequirePressureToActivate ? 1u : 0u;
    Config->RequirePressureContinuously = Config->RequirePressureContinuously ? 1u : 0u;
    Config->ForceTouchActionEnabled = Config->ForceTouchActionEnabled ? 1u : 0u;
    // Small-contact gates: two INDEPENDENT on/off flags (birth-only vs
    // continuous) - neither is forced off by the other being off. Each has
    // its own Major ("M") threshold, clamped to the same shared range.
    Config->SmallContactRejectionEnabled = Config->SmallContactRejectionEnabled ? 1u : 0u;
    Config->SmallContactBirthMajorThreshold = AmtClampULong(Config->SmallContactBirthMajorThreshold,
        AMT_POINTER_SMALL_CONTACT_MAJOR_MIN, AMT_POINTER_SMALL_CONTACT_MAJOR_MAX);
    Config->SmallContactRejectionStrict = Config->SmallContactRejectionStrict ? 1u : 0u;
    Config->SmallContactMajorThreshold = AmtClampULong(Config->SmallContactMajorThreshold,
        AMT_POINTER_SMALL_CONTACT_MAJOR_MIN, AMT_POINTER_SMALL_CONTACT_MAJOR_MAX);

    Config->ForceTouchSmallContactRejectionEnabled = Config->ForceTouchSmallContactRejectionEnabled ? 1u : 0u;
    Config->ForceTouchSmallContactBirthMajorThreshold = AmtClampULong(Config->ForceTouchSmallContactBirthMajorThreshold,
        AMT_POINTER_SMALL_CONTACT_MAJOR_MIN, AMT_POINTER_SMALL_CONTACT_MAJOR_MAX);
    Config->ForceTouchSmallContactRejectionStrict = Config->ForceTouchSmallContactRejectionStrict ? 1u : 0u;
    Config->ForceTouchSmallContactMajorThreshold = AmtClampULong(Config->ForceTouchSmallContactMajorThreshold,
        AMT_POINTER_SMALL_CONTACT_MAJOR_MIN, AMT_POINTER_SMALL_CONTACT_MAJOR_MAX);

    Config->ForceTouchEmulationEnabled = Config->ForceTouchEmulationEnabled ? 1u : 0u;
    if (Config->ForceTouchEmulationAction > AMT_POINTER_ACTION_MAX)
        Config->ForceTouchEmulationAction = AMT_POINTER_ACTION_CONTEXT_MENU;
    Config->ForceTouchEmulationHoldMs = AmtClampToStepGrid(
        Config->ForceTouchEmulationHoldMs,
        AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_MIN,
        AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_MAX,
        AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_STEP);

    Config->ForceTapDragLockoutDistance = AmtClampToStepGrid(
        Config->ForceTapDragLockoutDistance,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MIN,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MAX,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_STEP);

    Config->ForceTouchEmulationDragLockoutDistance = AmtClampToStepGrid(
        Config->ForceTouchEmulationDragLockoutDistance,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MIN,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MAX,
        AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_STEP);

    Config->CursorSmoothingPercent = AmtClampULong(Config->CursorSmoothingPercent, AMT_POINTER_SMOOTH_MIN, AMT_POINTER_SMOOTH_MAX);
    Config->CursorSpeedPercent = AmtClampULong(Config->CursorSpeedPercent, AMT_POINTER_SPEED_MIN, AMT_POINTER_SPEED_MAX);
    Config->CursorDeadzone = AmtClampULong(Config->CursorDeadzone, AMT_POINTER_DEADZONE_MIN, AMT_POINTER_DEADZONE_MAX);
    Config->CursorDeadzoneSlow = AmtClampULong(Config->CursorDeadzoneSlow, AMT_POINTER_DEADZONE_MIN, AMT_POINTER_DEADZONE_MAX);
    Config->CursorDeadzoneFast = AmtClampULong(Config->CursorDeadzoneFast, AMT_POINTER_DEADZONE_MIN, AMT_POINTER_DEADZONE_MAX);
    Config->CursorSlowVelocity = AmtClampULong(Config->CursorSlowVelocity, AMT_POINTER_SLOW_VEL_MIN, AMT_POINTER_SLOW_VEL_MAX);
    Config->CursorFastVelocity = AmtClampULong(Config->CursorFastVelocity, AMT_POINTER_FAST_VEL_MIN, AMT_POINTER_FAST_VEL_MAX);
    if (Config->CursorFastVelocity <= Config->CursorSlowVelocity)
        Config->CursorFastVelocity = Config->CursorSlowVelocity + 1;
    Config->SmoothingAlphaDen = AmtClampULong(Config->SmoothingAlphaDen, AMT_POINTER_ALPHA_DEN_MIN, AMT_POINTER_ALPHA_DEN_MAX);
    Config->SmoothingAlphaNumSlow = AmtClampULong(Config->SmoothingAlphaNumSlow, AMT_POINTER_ALPHA_SLOW_MIN, AMT_POINTER_ALPHA_SLOW_MAX);
    if (Config->SmoothingAlphaNumSlow > Config->SmoothingAlphaDen)
        Config->SmoothingAlphaNumSlow = Config->SmoothingAlphaDen;
}

VOID
AmtScrollConfigClamp(_Inout_ PAMT_SCROLL_CONFIG Config)
{
    Config->StructVersion = AMT_SCROLL_CONFIG_VERSION;
    Config->SpeedPercent = AmtClampULong(Config->SpeedPercent, AMT_SCROLL_SPEED_MIN, AMT_SCROLL_SPEED_MAX);
    Config->FastSpeedPercent = AmtClampULong(Config->FastSpeedPercent, AMT_SCROLL_FAST_SPEED_MIN, AMT_SCROLL_FAST_SPEED_MAX);
    Config->SmoothingPercent = AmtClampULong(Config->SmoothingPercent, AMT_SCROLL_SMOOTH_MIN, AMT_SCROLL_SMOOTH_MAX);
    Config->Deadzone = AmtClampULong(Config->Deadzone, AMT_SCROLL_DEADZONE_MIN, AMT_SCROLL_DEADZONE_MAX);
    Config->FastVelocity = AmtClampULong(Config->FastVelocity, AMT_SCROLL_FAST_VEL_MIN, AMT_SCROLL_FAST_VEL_MAX);
    Config->ScaleNum = AmtClampULong(Config->ScaleNum, AMT_SCROLL_SCALE_NUM_MIN, AMT_SCROLL_SCALE_NUM_MAX);
    Config->ScaleDen = AmtClampULong(Config->ScaleDen, AMT_SCROLL_SCALE_DEN_MIN, AMT_SCROLL_SCALE_DEN_MAX);
    Config->ScaleNumFast = AmtClampULong(Config->ScaleNumFast, AMT_SCROLL_SCALE_NUM_MIN, AMT_SCROLL_SCALE_NUM_MAX);
    Config->ScaleDenFast = AmtClampULong(Config->ScaleDenFast, AMT_SCROLL_SCALE_DEN_MIN, AMT_SCROLL_SCALE_DEN_MAX);
    RtlZeroMemory(Config->Reserved, sizeof(Config->Reserved));
}

// ----------------------------------------------------------------------------
// Registry persistence. Best-effort only: any failure (fresh install with
// no saved values yet, access-denied, corrupt/missing value) just leaves
// whatever the caller already had in *Config - which the caller has always
// pre-seeded with AMT_PALM_CONFIG_DEFAULT_INIT before calling Load.
// ----------------------------------------------------------------------------

static VOID
AmtRegistryReadDword(
    _In_ WDFKEY Key,
    _In_ PCWSTR ValueName,
    _Inout_ PULONG Value
)
{
    UNICODE_STRING valueName;
    ULONG          readValue = 0;
    NTSTATUS       status;

    RtlInitUnicodeString(&valueName, ValueName);
    status = WdfRegistryQueryULong(Key, &valueName, &readValue);
    if (NT_SUCCESS(status)) {
        *Value = readValue;
    }
    // else: leave *Value (the pre-seeded default) untouched.
}

static VOID
AmtRegistryWriteDword(
    _In_ WDFKEY Key,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value
)
{
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, ValueName);
    // Best-effort: ignore failure (e.g. read-only key on a locked-down
    // system) - the in-memory PalmConfig is already updated by the caller
    // regardless, so classification behavior is correct even if the save
    // silently doesn't stick across reboot.
    (VOID)WdfRegistryAssignULong(Key, &valueName, Value);
}

// ----------------------------------------------------------------------------
// Registry persistence. Each config struct is a flat run of ULONGs, so load
// and save are driven by one table per struct ({value name, member offset}).
// Adding a field to a config means adding ONE row here - a field can no
// longer be wired into Load but forgotten in Save (or vice versa), which is
// exactly how the drag-lockout distances once silently failed to persist.
// ----------------------------------------------------------------------------

typedef struct _AMT_REG_FIELD
{
    PCWSTR Name;
    ULONG  Offset;
} AMT_REG_FIELD;

#define AMT_REG_FIELD_ENTRY(Type, Member, Name) \
    { (Name), (ULONG)FIELD_OFFSET(Type, Member) }

static const AMT_REG_FIELD g_PalmRegFields[] = {
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, EdgePermilleTop, AMT_REG_VALUE_EDGE_TOP),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, EdgePermilleLeft, AMT_REG_VALUE_EDGE_LEFT),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, EdgePermilleRight, AMT_REG_VALUE_EDGE_RIGHT),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, EdgePermilleBottom, AMT_REG_VALUE_EDGE_BOTTOM),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmLargeMajor, AMT_REG_VALUE_LARGE_MAJOR),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmLargeRatio, AMT_REG_VALUE_LARGE_RATIO),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmScoreThresh, AMT_REG_VALUE_SCORE_THRESH),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmMinMajor, AMT_REG_VALUE_MIN_MAJOR),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmMinMinor, AMT_REG_VALUE_MIN_MINOR),
    AMT_REG_FIELD_ENTRY(AMT_PALM_CONFIG, PalmLockReleaseWhenGone, AMT_REG_VALUE_PALM_LOCK_RELEASE),
};

static const AMT_REG_FIELD g_PointerRegFields[] = {
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTapThreshold, AMT_REG_VALUE_FORCETAP_THRESHOLD),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTapAction, AMT_REG_VALUE_FORCETAP_ACTION),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchEnabled, AMT_REG_VALUE_FORCETOUCH_ENABLED),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, RequirePressureToActivate, AMT_REG_VALUE_REQUIRE_PRESSURE),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, RequirePressureContinuously, AMT_REG_VALUE_REQUIRE_PRESSURE_CONTINUOUS),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchActionEnabled, AMT_REG_VALUE_FORCETOUCH_ACTION_ENABLED),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorSmoothingPercent, AMT_REG_VALUE_CURSOR_SMOOTH),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorSpeedPercent, AMT_REG_VALUE_CURSOR_SPEED),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorDeadzone, AMT_REG_VALUE_CURSOR_DEADZONE),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorDeadzoneSlow, AMT_REG_VALUE_CURSOR_DEADZONE_SLOW),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorDeadzoneFast, AMT_REG_VALUE_CURSOR_DEADZONE_FAST),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorSlowVelocity, AMT_REG_VALUE_CURSOR_SLOW_VEL),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, CursorFastVelocity, AMT_REG_VALUE_CURSOR_FAST_VEL),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmoothingAlphaDen, AMT_REG_VALUE_CURSOR_ALPHA_DEN),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmoothingAlphaNumSlow, AMT_REG_VALUE_CURSOR_ALPHA_SLOW),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmallContactRejectionEnabled, AMT_REG_VALUE_SMALL_CONTACT_REJECTION),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmallContactRejectionStrict, AMT_REG_VALUE_SMALL_CONTACT_REJECTION_STRICT),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmallContactBirthMajorThreshold, AMT_REG_VALUE_SMALL_CONTACT_BIRTH_MAJOR_THRESHOLD),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, SmallContactMajorThreshold, AMT_REG_VALUE_SMALL_CONTACT_MAJOR_THRESHOLD),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchSmallContactRejectionEnabled, AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_REJECTION),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchSmallContactRejectionStrict, AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_REJECTION_STRICT),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchSmallContactBirthMajorThreshold, AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_BIRTH_MAJOR_THRESHOLD),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchSmallContactMajorThreshold, AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_MAJOR_THRESHOLD),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchEmulationEnabled, AMT_REG_VALUE_FORCETOUCH_EMULATION_ENABLED),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchEmulationAction, AMT_REG_VALUE_FORCETOUCH_EMULATION_ACTION),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchEmulationHoldMs, AMT_REG_VALUE_FORCETOUCH_EMULATION_HOLD_MS),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTapDragLockoutDistance, AMT_REG_VALUE_FORCE_TAP_DRAG_LOCKOUT_DISTANCE),
    AMT_REG_FIELD_ENTRY(AMT_POINTER_CONFIG, ForceTouchEmulationDragLockoutDistance, AMT_REG_VALUE_FORCETOUCH_EMULATION_DRAG_LOCKOUT_DISTANCE),
};

static const AMT_REG_FIELD g_ScrollRegFields[] = {
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, SpeedPercent, AMT_REG_VALUE_SCROLL_SPEED),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, FastSpeedPercent, AMT_REG_VALUE_SCROLL_FAST_SPEED),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, SmoothingPercent, AMT_REG_VALUE_SCROLL_SMOOTH),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, Deadzone, AMT_REG_VALUE_SCROLL_DEADZONE),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, FastVelocity, AMT_REG_VALUE_SCROLL_FAST_VEL),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, ScaleNum, AMT_REG_VALUE_SCROLL_SCALE_NUM),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, ScaleDen, AMT_REG_VALUE_SCROLL_SCALE_DEN),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, ScaleNumFast, AMT_REG_VALUE_SCROLL_SCALE_NUM_FAST),
    AMT_REG_FIELD_ENTRY(AMT_SCROLL_CONFIG, ScaleDenFast, AMT_REG_VALUE_SCROLL_SCALE_DEN_FAST),
};

// Open the device registry key and read every listed field into Config. A
// value missing from the registry leaves the pre-seeded default untouched.
// Returns FALSE (Config untouched) when the key cannot be opened - no saved
// values yet, or no access.
static BOOLEAN
AmtRegLoadFields(
    _In_ WDFDEVICE            Device,
    _Inout_ PVOID             Config,
    _In_reads_(Count) const AMT_REG_FIELD* Fields,
    _In_ size_t               Count
)
{
    WDFKEY key;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(
            Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return FALSE;
    }

    for (size_t i = 0; i < Count; i++) {
        AmtRegistryReadDword(key, Fields[i].Name,
                             (PULONG)((PUCHAR)Config + Fields[i].Offset));
    }

    WdfRegistryClose(key);
    return TRUE;
}

// Best-effort write of every listed field - the in-memory config is already
// applied by the caller regardless, so a failed open or write only means the
// choice does not survive a reboot. WdfDeviceOpenRegistryKey is
// PASSIVE_LEVEL-only; the SET/RESET IOCTL handlers always run there (default
// parallel WDFQUEUE, no DISPATCH_LEVEL constraint), which is why the public
// Save prototypes can stay annotated loosely for future callers.
static VOID
AmtRegSaveFields(
    _In_ WDFDEVICE            Device,
    _In_ const VOID*          Config,
    _In_reads_(Count) const AMT_REG_FIELD* Fields,
    _In_ size_t               Count
)
{
    WDFKEY key;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(
            Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    for (size_t i = 0; i < Count; i++) {
        AmtRegistryWriteDword(key, Fields[i].Name,
                              *(const ULONG*)((const UCHAR*)Config + Fields[i].Offset));
    }

    WdfRegistryClose(key);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtPalmConfigLoadFromRegistry(
    _In_ WDFDEVICE Device,
    _Inout_ PAMT_PALM_CONFIG Config
)
{
    PAGED_CODE();

    // A value loaded straight from the registry is untrusted input the same
    // as anything arriving over the SET IOCTL - clamp it too.
    if (AmtRegLoadFields(Device, Config, g_PalmRegFields, ARRAYSIZE(g_PalmRegFields)))
        AmtPalmConfigClamp(Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPalmConfigSaveToRegistry(
    _In_ WDFDEVICE Device,
    _In_ const AMT_PALM_CONFIG* Config
)
{
    AmtRegSaveFields(Device, Config, g_PalmRegFields, ARRAYSIZE(g_PalmRegFields));
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtPointerConfigLoadFromRegistry(
    _In_ WDFDEVICE Device,
    _Inout_ PAMT_POINTER_CONFIG Config
)
{
    PAGED_CODE();

    // A value loaded straight from the registry is untrusted input the same
    // as anything arriving over the SET IOCTL - clamp it too.
    if (AmtRegLoadFields(Device, Config, g_PointerRegFields, ARRAYSIZE(g_PointerRegFields)))
        AmtPointerConfigClamp(Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPointerConfigSaveToRegistry(
    _In_ WDFDEVICE Device,
    _In_ const AMT_POINTER_CONFIG* Config
)
{
    AmtRegSaveFields(Device, Config, g_PointerRegFields, ARRAYSIZE(g_PointerRegFields));
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtScrollConfigLoadFromRegistry(
    _In_ WDFDEVICE Device,
    _Inout_ PAMT_SCROLL_CONFIG Config
)
{
    PAGED_CODE();

    // A value loaded straight from the registry is untrusted input the same
    // as anything arriving over the SET IOCTL - clamp it too.
    if (AmtRegLoadFields(Device, Config, g_ScrollRegFields, ARRAYSIZE(g_ScrollRegFields)))
        AmtScrollConfigClamp(Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtScrollConfigSaveToRegistry(
    _In_ WDFDEVICE Device,
    _In_ const AMT_SCROLL_CONFIG* Config
)
{
    AmtRegSaveFields(Device, Config, g_ScrollRegFields, ARRAYSIZE(g_ScrollRegFields));
}

// ----------------------------------------------------------------------------
// IOCTL handlers - dispatched from Queue.c's EvtIoDeviceControl.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Shared plumbing for the Get/Set config IOCTLs. The palm, pointer and scroll
// handlers differ only in the config type, its clamp/rebuild/save calls and
// which context field they touch; everything else (buffer retrieval,
// StateLock-guarded copy, echo of the clamped result) lives here once.
// ----------------------------------------------------------------------------

// Copy a config out to the caller's output buffer under StateLock.
static NTSTATUS
AmtCfgGet(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ WDFREQUEST      Request,
    _In_ const VOID*     Src,
    _In_ size_t          Size
)
{
    PVOID    out;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, Size, &out, NULL);
    if (!NT_SUCCESS(status))
        return status;

    WdfSpinLockAcquire(ctx->StateLock);
    RtlCopyMemory(out, Src, Size);
    WdfSpinLockRelease(ctx->StateLock);

    WdfRequestSetInformation(Request, Size);
    return STATUS_SUCCESS;
}

// Copy the caller's input buffer into Dest before anything validates or
// applies it - the buffer is still user-influenced, never trust it in place.
static NTSTATUS
AmtCfgReadInput(
    _In_  WDFREQUEST Request,
    _In_  size_t     Size,
    _Out_ PVOID      Dest
)
{
    PVOID    in;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, Size, &in, NULL);
    if (!NT_SUCCESS(status))
        return status;

    RtlCopyMemory(Dest, in, Size);
    return STATUS_SUCCESS;
}

// Echo the clamped values back (best-effort) so the GUI immediately reflects
// any adjustment the driver made to an out-of-range input, instead of a
// slider silently drifting from what is actually in effect.
static VOID
AmtCfgEcho(
    _In_ WDFREQUEST  Request,
    _In_ const VOID* Src,
    _In_ size_t      Size
)
{
    PVOID out;
    if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, Size, &out, NULL))) {
        RtlCopyMemory(out, Src, Size);
        WdfRequestSetInformation(Request, Size);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    return AmtCfgGet(ctx, Request, &ctx->PalmConfig, sizeof(AMT_PALM_CONFIG));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpSetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    AMT_PALM_CONFIG clamped;

    NTSTATUS status = AmtCfgReadInput(Request, sizeof(clamped), &clamped);
    if (!NT_SUCCESS(status))
        return status;

    // Clamp every field into a safe range - never trust the GUI (or anything
    // else calling this IOCTL) to have done that itself.
    AmtPalmConfigClamp(&clamped);

    WdfSpinLockAcquire(ctx->StateLock);
    ctx->PalmConfig = clamped;
    AmtPalmRuntimeRebuild(&clamped, &ctx->PalmRuntime);
    WdfSpinLockRelease(ctx->StateLock);

    // Persist so the choice survives a reboot/replug, same as an Elan/
    // Synaptics OEM panel's "Apply" button. Best-effort - see
    // AmtPalmConfigSaveToRegistry.
    AmtPalmConfigSaveToRegistry(Device, &clamped);

    AmtCfgEcho(Request, &clamped, sizeof(clamped));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetPadGeometry(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS          status;
    PDEVICE_CONTEXT   pDeviceContext;
    PAMT_PAD_GEOMETRY pOutGeometry;
    size_t            outLen = 0;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(AMT_PAD_GEOMETRY), (PVOID*)&pOutGeometry, &outLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    // WdfRequestRetrieveOutputBuffer already enforced outLen >=
    // sizeof(AMT_PAD_GEOMETRY) at runtime, but that relationship isn't
    // visible to static analysis from outLen alone - check it explicitly
    // so PREfast can see the RtlZeroMemory/writes below are in-bounds
    // (fixes C6386 buffer-overrun warning on pOutGeometry).
    if (outLen < sizeof(AMT_PAD_GEOMETRY)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto exit;
    }

    if (pDeviceContext->DeviceInfo == NULL) {
        // Hardware not enumerated yet (queried before EvtDevicePrepareHardware
        // finished, or device in a low-power/removed state) - report a
        // clearly-invalid-but-safe zero range instead of dereferencing NULL.
        // The GUI's preview mode treats XMax<=XMin as "use fallback pad size".
        RtlZeroMemory(pOutGeometry, sizeof(AMT_PAD_GEOMETRY));
        pOutGeometry->StructVersion = AMT_PAD_GEOMETRY_VERSION;
        status = STATUS_SUCCESS;
        goto exit;
    }

    pOutGeometry->StructVersion = AMT_PAD_GEOMETRY_VERSION;
    pOutGeometry->XMin = pDeviceContext->DeviceInfo->x.min;
    pOutGeometry->XMax = pDeviceContext->DeviceInfo->x.max;
    pOutGeometry->YMin = pDeviceContext->DeviceInfo->y.min;
    pOutGeometry->YMax = pDeviceContext->DeviceInfo->y.max;

    WdfRequestSetInformation(Request, sizeof(AMT_PAD_GEOMETRY));
    status = STATUS_SUCCESS;

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpResetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(Device);
    AMT_PALM_CONFIG defaults = AMT_PALM_CONFIG_DEFAULT_INIT;

    UNREFERENCED_PARAMETER(Request); // no in/out buffer for this IOCTL

    WdfSpinLockAcquire(pDeviceContext->StateLock);
    pDeviceContext->PalmConfig = defaults;
    AmtPalmRuntimeRebuild(&defaults, &pDeviceContext->PalmRuntime);
    WdfSpinLockRelease(pDeviceContext->StateLock);

    AmtPalmConfigSaveToRegistry(Device, &defaults);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    return AmtCfgGet(ctx, Request, &ctx->PointerConfig, sizeof(AMT_POINTER_CONFIG));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpSetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT    ctx = DeviceGetContext(Device);
    AMT_POINTER_CONFIG clamped;

    NTSTATUS status = AmtCfgReadInput(Request, sizeof(clamped), &clamped);
    if (!NT_SUCCESS(status))
        return status;

    // Same pattern as AmtPtpSetPalmConfig: clamp, apply, persist, echo.
    AmtPointerConfigClamp(&clamped);

    WdfSpinLockAcquire(ctx->StateLock);
    ctx->PointerConfig = clamped;
    AmtPointerRuntimeRebuild(&clamped, &ctx->PointerRuntime);
    AmtPointerForceTouchTimingRebuild(ctx);
    WdfSpinLockRelease(ctx->StateLock);

    AmtPointerConfigSaveToRegistry(Device, &clamped);

    AmtCfgEcho(Request, &clamped, sizeof(clamped));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpResetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT    pDeviceContext = DeviceGetContext(Device);
    AMT_POINTER_CONFIG defaults = AMT_POINTER_CONFIG_DEFAULT_INIT;

    UNREFERENCED_PARAMETER(Request); // no in/out buffer for this IOCTL

    WdfSpinLockAcquire(pDeviceContext->StateLock);
    pDeviceContext->PointerConfig = defaults;
    AmtPointerRuntimeRebuild(&defaults, &pDeviceContext->PointerRuntime);
    AmtPointerForceTouchTimingRebuild(pDeviceContext);
    WdfSpinLockRelease(pDeviceContext->StateLock);

    AmtPointerConfigSaveToRegistry(Device, &defaults);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    return AmtCfgGet(ctx, Request, &ctx->ScrollConfig, sizeof(AMT_SCROLL_CONFIG));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpSetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT   ctx = DeviceGetContext(Device);
    AMT_SCROLL_CONFIG clamped;

    NTSTATUS status = AmtCfgReadInput(Request, sizeof(clamped), &clamped);
    if (!NT_SUCCESS(status))
        return status;

    AmtScrollConfigClamp(&clamped);

    WdfSpinLockAcquire(ctx->StateLock);
    ctx->ScrollConfig = clamped;
    AmtScrollRuntimeRebuild(&clamped, &ctx->ScrollRuntime);
    WdfSpinLockRelease(ctx->StateLock);

    AmtScrollConfigSaveToRegistry(Device, &clamped);

    AmtCfgEcho(Request, &clamped, sizeof(clamped));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpResetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    AMT_SCROLL_CONFIG defaults = AMT_SCROLL_CONFIG_DEFAULT_INIT;
    UNREFERENCED_PARAMETER(Request);
    WdfSpinLockAcquire(ctx->StateLock);
    ctx->ScrollConfig = defaults;
    AmtScrollRuntimeRebuild(&defaults, &ctx->ScrollRuntime);
    WdfSpinLockRelease(ctx->StateLock);
    AmtScrollConfigSaveToRegistry(Device, &defaults);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetDeviceInfo(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS         status;
    PDEVICE_CONTEXT  pDeviceContext;
    PAMT_DEVICE_INFO pOutInfo;
    size_t           outLen = 0;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(AMT_DEVICE_INFO), (PVOID*)&pOutInfo, &outLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    if (outLen < sizeof(AMT_DEVICE_INFO)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto exit;
    }

    RtlZeroMemory(pOutInfo, sizeof(AMT_DEVICE_INFO));
    pOutInfo->StructVersion = AMT_DEVICE_INFO_VERSION;
    // VendorId/ProductId are 0 (and SupportsForceTouch left FALSE) if
    // EvtDevicePrepareHardware hasn't run yet - the GUI already treats
    // ProductId==0 as "no device info" and falls back to the SMBIOS-name
    // path, so this is a safe default rather than dereferencing anything
    // that might not be populated yet.
    pOutInfo->VendorId = pDeviceContext->DeviceDescriptor.idVendor;
    pOutInfo->ProductId = pDeviceContext->DeviceDescriptor.idProduct;
    pOutInfo->SupportsForceTouch = pDeviceContext->SupportsForceTouch ? 1 : 0;

    WdfRequestSetInformation(Request, sizeof(AMT_DEVICE_INFO));
    status = STATUS_SUCCESS;

exit:
    return status;
}

// ----------------------------------------------------------------------------
// Runtime debug-trace switch (see Trace.h/Trace.c). GET/SET a plain ULONG
// (0/1), same shape as AmtPtpSetLiveEnabled but with no per-file ownership
// tracking - unlike Live, any GUI instance may read or flip this, and it
// persists to the registry immediately on SET so it survives a
// reboot/replug (mirrors AmtPtpSetPalmConfig's "apply then best-effort
// save" sequence).
// ----------------------------------------------------------------------------

NTSTATUS
AmtPtpGetDebugMode(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT pDeviceContext;
    PULONG          pOutValue;
    size_t          outLen = 0;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ULONG), (PVOID*)&pOutValue, &outLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    *pOutValue = pDeviceContext->TraceDebugEnabled ? 1u : 0u;

    WdfRequestSetInformation(Request, sizeof(ULONG));
    status = STATUS_SUCCESS;

exit:
    return status;
}

NTSTATUS
AmtPtpSetDebugMode(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT pDeviceContext;
    PULONG          pInValue;
    size_t          inLen = 0;
    WDFKEY          key;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(ULONG), (PVOID*)&pInValue, &inLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    // Applies immediately - the very next AmtTrace() call site (any
    // thread, any IRQL <= DISPATCH_LEVEL) sees the new value. No lock
    // needed: a single BOOLEAN write/read has no torn-value hazard on any
    // architecture this driver targets, and a trace line landing on the
    // "wrong" side of a flip by one call site is harmless.
    pDeviceContext->TraceDebugEnabled = (*pInValue != 0);

    // Best-effort persistence, same as AmtPalmConfigSaveToRegistry - the
    // in-memory switch above already took effect regardless of whether
    // this succeeds.
    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(status)) {
        AmtRegistryWriteDword(key, AMT_TRACE_DEBUGMODE_VALUE_NAME,
            pDeviceContext->TraceDebugEnabled ? 1u : 0u);
        WdfRegistryClose(key);
    }

    WdfRequestSetInformation(Request, 0);
    status = STATUS_SUCCESS;

exit:
    return status;
}

// ----------------------------------------------------------------------------
// Control-device dispatch - called for IOCTLs arriving through
// \\.\\AmtPtpDeviceUsbKm. The control device itself has no DEVICE_CONTEXT;
// its small context stores the PnP filter FDO that owns the real state.
// ----------------------------------------------------------------------------

VOID
AmtPtpConfigControlEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFDEVICE controlDevice;
    WDFDEVICE targetDevice;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    controlDevice = WdfIoQueueGetDevice(Queue);

    // BUG FIX: this used to read controlContext->TargetDevice directly,
    // with no lock - the same unguarded-read hazard the D0ExitLock/
    // RecoveryLock pattern elsewhere in this driver exists to close. A
    // sleep-triggered surprise-removal/re-enumeration nulls (or re-points)
    // this field from a different thread under ConfigControlDeviceLock;
    // reading it here without that lock could hand a handler a
    // NULL/stale WDFDEVICE it then dereferences via DeviceGetContext.
    // AmtPtpConfigControlSnapshotTargetDevice takes the one lock every
    // writer already uses.
    //
    // BUG FIX (use-after-free): this is THE actual GET_CONFIG/SET_CONFIG
    // race - the field-read TOCTOU above was closed, but nothing kept the
    // target FDO's object memory alive for the rest of this function. Every
    // GET_*_CONFIG/SET_*_CONFIG/GET_PAD_GEOMETRY/GET_DEVICE_INFO/
    // GET_DEBUG_MODE/SET_DEBUG_MODE handler below is called with this same
    // targetDevice and reaches it via DeviceGetContext(); SET_*_CONFIG in
    // particular does blocking PASSIVE_LEVEL registry I/O
    // (AmtPalmConfigSaveToRegistry / AmtPointerConfigSaveToRegistry /
    // AmtScrollConfigSaveToRegistry), which is a wide window for a
    // concurrent surprise removal to finish tearing the FDO down and free
    // its DEVICE_CONTEXT out from under a handler still using it.
    // AmtPtpConfigControlSnapshotTargetDevice now takes a reference that
    // keeps the FDO allocated regardless of how far PnP removal has
    // otherwise progressed; it is released once, below, after every
    // handler in the switch has finished with targetDevice.
    targetDevice = AmtPtpConfigControlSnapshotTargetDevice(controlDevice);

    if (targetDevice == NULL) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    switch (IoControlCode)
    {
    case IOCTL_AMT_PTP_GET_PALM_CONFIG:
        status = AmtPtpGetPalmConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_SET_PALM_CONFIG:
        status = AmtPtpSetPalmConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_PAD_GEOMETRY:
        status = AmtPtpGetPadGeometry(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_RESET_PALM_CONFIG:
        status = AmtPtpResetPalmConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_POINTER_CONFIG:
        status = AmtPtpGetPointerConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_SET_POINTER_CONFIG:
        status = AmtPtpSetPointerConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_RESET_POINTER_CONFIG:
        status = AmtPtpResetPointerConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_SCROLL_CONFIG:
        status = AmtPtpGetScrollConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_SET_SCROLL_CONFIG:
        status = AmtPtpSetScrollConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_RESET_SCROLL_CONFIG:
        status = AmtPtpResetScrollConfig(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_SET_LIVE_ENABLED:
        status = AmtPtpSetLiveEnabled(controlDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_LIVE_FRAME:
        status = AmtPtpGetLiveFrame(controlDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_DEVICE_INFO:
        status = AmtPtpGetDeviceInfo(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_GET_DEBUG_MODE:
        status = AmtPtpGetDebugMode(targetDevice, Request);
        break;

    case IOCTL_AMT_PTP_SET_DEBUG_MODE:
        status = AmtPtpSetDebugMode(targetDevice, Request);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    // Release the reference taken above - every handler in the switch has
    // returned by this point, so targetDevice is done being used.
    AmtPtpConfigControlReleaseTargetDevice(targetDevice);

    WdfRequestComplete(Request, status);
}


// ----------------------------------------------------------------------------
// Live touch monitor
//
// Explicitly opt-in. SET_LIVE_ENABLED toggles a cheap boolean. While false,
// Interrupt.c does not build/copy a live snapshot at all. While true, the
// Latest processed frame is copied into DeviceContext->LiveFrame under the
// dedicated LiveLock so frame processing does not hold the long StateLock. The GUI
// polls GET_LIVE_FRAME only while its Live checkbox is checked.
// ----------------------------------------------------------------------------

NTSTATUS
AmtPtpSetLiveEnabled(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PAMT_CONFIG_CONTROL_FILE_CONTEXT fileContext;
    PDEVICE_CONTEXT targetContext;
    WDFDEVICE targetDevice;
    WDFFILEOBJECT fileObject;
    PULONG enabled;
    size_t inputLength = 0;
    NTSTATUS status;

    fileObject = WdfRequestGetFileObject(Request);
    fileContext = fileObject != NULL
        ? AmtConfigControlFileGetContext(fileObject)
        : NULL;

    // BUG FIX: was two separate unguarded reads of
    // controlContext->TargetDevice (a NULL-check here, then a dereference
    // via DeviceGetContext below) - a classic check-then-use TOCTOU on a
    // field a concurrent sleep/wake re-enumeration can null out between
    // the two. One locked snapshot closes that window.
    //
    // BUG FIX (use-after-free): the snapshot now also holds a reference on
    // targetDevice (see AmtPtpConfigControlSnapshotTargetDevice) that MUST
    // be released on every exit path below - hence the single goto-exit
    // instead of the scattered early returns this function used to have.
    targetDevice = AmtPtpConfigControlSnapshotTargetDevice(Device);

    if (targetDevice == NULL || fileContext == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto exit;
    }

    targetContext = DeviceGetContext(targetDevice);

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(ULONG), (PVOID*)&enabled, &inputLength);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    WdfSpinLockAcquire(targetContext->LiveLock);

    if (*enabled != 0) {
        if (targetContext->LiveOwnerFileObject != NULL &&
            targetContext->LiveOwnerFileObject != fileObject) {
            WdfSpinLockRelease(targetContext->LiveLock);
            status = STATUS_SHARING_VIOLATION;
            goto exit;
        }

        targetContext->LiveOwnerFileObject = fileObject;
        fileContext->LiveOwner = TRUE;
        InterlockedExchange(&targetContext->LiveEnabled, 1);
        targetContext->LiveSequence = 0;
        RtlZeroMemory(&targetContext->LiveFrame, sizeof(targetContext->LiveFrame));
        targetContext->LiveFrame.StructVersion = AMT_LIVE_FRAME_VERSION;
    } else {
        if (targetContext->LiveOwnerFileObject != fileObject) {
            WdfSpinLockRelease(targetContext->LiveLock);
            status = STATUS_ACCESS_DENIED;
            goto exit;
        }

        InterlockedExchange(&targetContext->LiveEnabled, 0);
        targetContext->LiveOwnerFileObject = NULL;
        fileContext->LiveOwner = FALSE;
    }

    WdfSpinLockRelease(targetContext->LiveLock);

    WdfRequestSetInformation(Request, 0);
    status = STATUS_SUCCESS;

exit:
    AmtPtpConfigControlReleaseTargetDevice(targetDevice);
    return status;
}

NTSTATUS
AmtPtpGetLiveFrame(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PAMT_CONFIG_CONTROL_FILE_CONTEXT fileContext;
    PDEVICE_CONTEXT targetContext;
    WDFDEVICE targetDevice;
    PAMT_LIVE_FRAME output;
    size_t outputLength = 0;
    NTSTATUS status;

    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    fileContext = fileObject != NULL
        ? AmtConfigControlFileGetContext(fileObject)
        : NULL;

    // BUG FIX: same TOCTOU as AmtPtpSetLiveEnabled, but the highest-traffic
    // instance of it - the GUI calls this at up to 30 Hz for the whole
    // time its Live preview is on, so the old check-then-use window
    // (controlContext->TargetDevice == NULL check, then a second unguarded
    // read of the same field for DeviceGetContext a few lines later) got
    // exercised far more often than the one-shot SetLiveEnabled/dispatcher
    // reads - and lines up exactly with the sleep-triggered
    // surprise-removal/re-enumeration window this driver already goes to
    // such lengths to synchronize against elsewhere (D0ExitLock/
    // RecoveryLock). One locked snapshot, used once.
    //
    // BUG FIX (use-after-free): being the highest-frequency caller of
    // AmtPtpConfigControlSnapshotTargetDevice also makes this the call site
    // most likely to actually hit the reference-less UAF window against a
    // torn-down FDO in practice - so it gets the same goto-exit/paired
    // release treatment as AmtPtpSetLiveEnabled.
    targetDevice = AmtPtpConfigControlSnapshotTargetDevice(Device);

    if (targetDevice == NULL || fileContext == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto exit;
    }

    targetContext = DeviceGetContext(targetDevice);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(AMT_LIVE_FRAME), (PVOID*)&output, &outputLength);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    WdfSpinLockAcquire(targetContext->LiveLock);

    if (!fileContext->LiveOwner ||
        targetContext->LiveOwnerFileObject != fileObject) {
        WdfSpinLockRelease(targetContext->LiveLock);
        status = STATUS_ACCESS_DENIED;
        goto exit;
    }

    if (InterlockedCompareExchange(&targetContext->LiveEnabled, 0, 0) == 0) {
        WdfSpinLockRelease(targetContext->LiveLock);
        status = STATUS_DEVICE_NOT_READY;
        goto exit;
    }

    *output = targetContext->LiveFrame;

    WdfSpinLockRelease(targetContext->LiveLock);

    WdfRequestSetInformation(Request, sizeof(AMT_LIVE_FRAME));
    status = STATUS_SUCCESS;

exit:
    AmtPtpConfigControlReleaseTargetDevice(targetDevice);
    return status;
}

// ----------------------------------------------------------------------------
// AmtPtpConfigControlEvtFileClose
//
// Safety net for AmtPtpSetLiveEnabled(FALSE): fires when the handle to
// \\DosDevices\\AmtPtpDeviceUsbKm closes for ANY reason, not just a clean
// GUI shutdown. The GUI's own Closed handler already disables Live on a
// normal exit, but a crash, taskkill, unplug, or BSOD skips that handler
// entirely and would otherwise leave LiveEnabled stuck TRUE - the interrupt
// hot path would keep building and copying live snapshots forever for a
// monitor that no longer exists, with no consumer ever calling
// GET_LIVE_FRAME again. Forcing it off here costs nothing on the normal-exit
// path (SetLiveEnabled(FALSE) already ran, so this is just a redundant
// FALSE-to-FALSE write) and closes the leak on every other exit path.
// ----------------------------------------------------------------------------

VOID
AmtPtpConfigControlEvtFileClose(_In_ WDFFILEOBJECT FileObject)
{
    WDFDEVICE                   controlDevice;
    PAMT_CONFIG_CONTROL_FILE_CONTEXT fileContext;
    PDEVICE_CONTEXT             targetContext;
    WDFDEVICE                   targetDevice;

    controlDevice = WdfFileObjectGetDevice(FileObject);
    fileContext = AmtConfigControlFileGetContext(FileObject);

    // BUG FIX: same unlocked check-then-use of TargetDevice as the other
    // handlers in this file - a handle can close (crash, taskkill, unplug)
    // at the same moment a sleep-triggered re-enumeration is nulling this
    // field out, and the old code read it twice, unlocked, to get there.
    //
    // BUG FIX (use-after-free): paired with
    // AmtPtpConfigControlReleaseTargetDevice below - see
    // AmtPtpConfigControlSnapshotTargetDevice's comment in Device.c.
    targetDevice = AmtPtpConfigControlSnapshotTargetDevice(controlDevice);

    if (fileContext == NULL || !fileContext->LiveOwner ||
        targetDevice == NULL) {
        AmtPtpConfigControlReleaseTargetDevice(targetDevice);
        return;
    }

    targetContext = DeviceGetContext(targetDevice);

    WdfSpinLockAcquire(targetContext->LiveLock);
    if (targetContext->LiveOwnerFileObject == FileObject) {
        InterlockedExchange(&targetContext->LiveEnabled, 0);
        targetContext->LiveOwnerFileObject = NULL;
    }
    fileContext->LiveOwner = FALSE;
    WdfSpinLockRelease(targetContext->LiveLock);

    AmtPtpConfigControlReleaseTargetDevice(targetDevice);
}

VOID
AmtPtpConfigControlEvtConfigControlCleanup(_In_ WDFOBJECT Object)
{
    WDFDEVICE controlDevice = (WDFDEVICE)Object;
    PAMT_CONFIG_CONTROL_CONTEXT controlContext =
        AmtConfigControlGetContext(controlDevice);

    controlContext->TargetDevice = NULL;
}