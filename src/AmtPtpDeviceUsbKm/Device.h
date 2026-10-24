// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "PTPCore.h"
#include "Palm.h"

EXTERN_C_START

// Hold the ordinary click until the press is classified: gates
// *OutButtonClickReport (see PTPCore_ProcessFrame) on
// CLICK_ARBITRATION_HARD_TAP (or a fast release before resolving at all).
// A press that resolves to CLICK_ARBITRATION_FORCE_TOUCH instead NEVER
// reports the ordinary click - force touch (hardware pressure OR emulated
// hold-duration) replaces it for that press, it does not run alongside it.
// Reporting both used to mean an existing text selection got clobbered by
// the ordinary click's own down+up landing right before the force-touch
// action's pulse (see forceTouchFiredThisFrame in PTPCore_ProcessFrame).
//
// The dead zone this creates for an actually-dragging press is bounded by
// the (small, configurable) drag-lockout distance, not by
// CLICK_ARBITRATION_TIMEOUT_MS/ForceTouchEmulationHoldMs: tripping the
// lockout resolves to HARD_TAP immediately, same frame - see the REWORK
// note on drag lockout in PTPCore_ProcessFrame.
typedef enum _CLICK_ARBITRATION_STATE
{
    CLICK_ARBITRATION_IDLE = 0,    // button not down
    CLICK_ARBITRATION_PENDING,     // button down, still deciding
    CLICK_ARBITRATION_HARD_TAP,    // decided: ordinary click - report it
    CLICK_ARBITRATION_FORCE_TOUCH  // decided: force touch (pulse already fired on this transition - see forceTouchFiredThisFrame) - ordinary click never reported for this press
} CLICK_ARBITRATION_STATE;

// Synthetic right-click delivery for force-touch clicks.
//
// REWORKED: previously a flat ring of individual Button2 edges (down/up
// treated as interchangeable entries), evicted from the head on overflow.
// That could orphan an already-delivered DOWN when its matching UP got
// evicted before delivery (e.g. several force-touch clicks landing
// back-to-back while mouhid.sys's read cadence lagged) - Windows would
// then see Button2 latched down with no UP ever coming: a stuck
// right-click. Split into two pieces so that failure mode is structurally
// impossible:
//
//  - ForceTouchDeliveryState: the ONE click currently being delivered.
//    Its UP is always guaranteed to be sent (eventually) before a new
//    click's DOWN is ever started - so a DOWN can never reach the host
//    without its UP following.
//  - PendingForceTouchClickCount: clicks still waiting to START. These
//    have delivered nothing yet, so dropping the oldest on overflow (a
//    very fast click storm outrunning available mouse read requests) is
//    always safe - at worst a click gets silently coalesced away, never
//    a stuck button.
typedef enum _FORCE_TOUCH_DELIVERY_STATE
{
    FORCE_TOUCH_DELIVERY_IDLE = 0,     // nothing in flight
    FORCE_TOUCH_DELIVERY_DOWN_PENDING, // DOWN not yet delivered
    FORCE_TOUCH_DELIVERY_UP_PENDING    // DOWN delivered, UP not yet delivered
} FORCE_TOUCH_DELIVERY_STATE;

// Escalation ladder for interrupt-pipe read failures, matching the USB
// client-driver recovery guidance in the Windows USB driver design
// documentation: try the least disruptive recovery first and only escalate
// if the pipe is still failing afterward.
//
//   1. reset-pipe  - WdfUsbTargetPipeResetSynchronously: clears a halted/
//                    stalled endpoint without touching the rest of the
//                    device.
//   2. reset-port  - WdfUsbTargetDeviceResetPortSynchronously: a full
//                    device reset via the parent hub port; the framework
//                    reselects the current USB configuration afterward, so
//                    existing pipe handles remain valid.
//   3. cycle-port  - IOCTL_INTERNAL_USB_CYCLE_PORT (the raw request behind
//                    WdfUsbTargetDeviceCyclePortSynchronously - see
//                    AmtPtpCyclePort for why the raw IOCTL is used
//                    instead): power-cycles the port, i.e. treats the
//                    device exactly like an unplug/replug. This is the
//                    last resort - the device will disappear and reappear
//                    through normal PnP.
//
// All three rungs require WdfIoTargetStop on the interrupt pipe's I/O
// target first, per the documented preconditions of each call above; see
// AmtPtpEvtReaderRestartTimer.
//
// Each stage is attempted at most once per D0 session before escalating to
// the next; once cycle-port has been tried, this driver instance gives up
// (READER_RECOVERY_EXHAUSTED) until the next D0Entry or PnP re-arrival.
typedef enum _READER_RECOVERY_STAGE
{
    READER_RECOVERY_RESET_PIPE = 0,
    READER_RECOVERY_RESET_PORT,
    READER_RECOVERY_CYCLE_PORT,
    READER_RECOVERY_EXHAUSTED
} READER_RECOVERY_STAGE;

typedef struct _AMT_CONFIG_CONTROL_CONTEXT
{
    // The PnP filter device that owns the actual palm/geometry state.
    // This is a WDF handle, not a raw WDM pointer.
    //
    // IMPORTANT: this field itself holds NO reference on the FDO - it is
    // just a pointer-sized value guarded by ConfigControlDeviceLock against
    // torn/concurrent read-write (see AmtPtpConfigControlSnapshotTargetDevice).
    // A lock-protected READ of this field is not the same thing as keeping
    // the FDO alive for however long the caller goes on to use the handle
    // afterward - the lock is released before any handler runs. Every
    // caller that snapshots this field MUST pair it with
    // AmtPtpConfigControlReleaseTargetDevice() once it is done using the
    // returned handle; see that function pair's comment for why.
    WDFDEVICE TargetDevice;
} AMT_CONFIG_CONTROL_CONTEXT, *PAMT_CONFIG_CONTROL_CONTEXT;

typedef struct _AMT_CONFIG_CONTROL_FILE_CONTEXT
{
    // Only the file object that successfully enabled Live owns the active
    // live session. Closing another handle must never disable another
    // client's session.
    BOOLEAN LiveOwner;
} AMT_CONFIG_CONTROL_FILE_CONTEXT, *PAMT_CONFIG_CONTROL_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
    AMT_CONFIG_CONTROL_FILE_CONTEXT,
    AmtConfigControlFileGetContext
)

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
    AMT_CONFIG_CONTROL_CONTEXT,
    AmtConfigControlGetContext
)


typedef struct _DEVICE_CONTEXT
{
    // Protect shared frame-processing state across concurrent USB completions.
    WDFSPINLOCK     StateLock;

    // Live-monitor state has its own short lock so the USB frame-processing
    // critical section does not include live snapshot publication.
    WDFSPINLOCK     LiveLock;

    // USB - touched every interrupt/report cycle. Governed by the
    // two-level D0ExitLock/RecoveryLock model documented below: every
    // reader of UsbDevice/InterruptPipe/UsbInterface either takes only a
    // short D0ExitLock snapshot, or - for any actual (possibly blocking)
    // use of the handle - holds RecoveryLock across that use, matching
    // the same lock AmtPtpEvtDeviceD0Exit/AmtPtpEvtDeviceReleaseHardware
    // take before nulling these fields. AmtPtpSetFeatures's
    // AmtPtpSetWellspringMode retry loop (Hid.c) is a RecoveryLock holder
    // for this reason, not just the D0Entry/D0Exit paths.
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;

    // Separate HID read queues for the digitizer and the force-touch mouse
    // top-level collections. Queue.c classifies READ_REPORT requests by the
    // caller-provided output-buffer size before forwarding them.
    WDFQUEUE        InputQueue;

    // Latest digitizer report when no HID READ is queued. Coalesced to
    // prevent USB processing from depending on HIDClass read timing.
    // Protected by StateLock; reset at every D0Entry.
    PTP_REPORT      PendingTouchReport;
    BOOLEAN         PendingTouchReportValid;

    // NOTE: the GUI's config control device is intentionally NOT a field
    // here. It is driver-lifetime state (DRIVER_CONTEXT::ConfigControlDevice
    // in Driver.h), created once and reattached to whichever FDO is
    // current via AmtPtpAcquireConfigControlDevice - see that function's
    // comment in Device.c for why a per-FDO control device is unsafe
    // across surprise removal/re-enumeration.

    // Opt-in live monitor state. FALSE is the normal/idle state and the
    // interrupt path does not copy any live-monitor data when it is false.
    volatile LONG   LiveEnabled;
    WDFFILEOBJECT   LiveOwnerFileObject;
    ULONG           LiveSequence;

    // Single buffer - both the writer (Interrupt.c) and every reader
    // (ConfigIoctl.c: SET_LIVE_ENABLED's reset, GET_LIVE_FRAME) hold
    // LiveLock across their entire access, so the lock alone already
    // gives full exclusion and cross-CPU visibility. Do NOT remove that
    // locking (e.g. to make GET_LIVE_FRAME lock-free for latency) without
    // first reintroducing double-buffering or another synchronization
    // scheme here - without it, a reader could observe a partially-
    // written frame.
    AMT_LIVE_FRAME  LiveFrame;

    // Device config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN IsWellspringModeOn;

    // Runtime debug-trace switch (see Trace.h/Trace.c). Loaded once from
    // this device's "DebugMode" REG_DWORD in AmtPtpDeviceUsbKmCreateDevice;
    // FALSE (traces compiled out to a single bool check, nothing printed)
    // unless that registry value is present and non-zero. Unrelated to
    // LiveEnabled below - that is the GUI's always-available live-frame
    // monitor, not a debug facility.
    BOOLEAN TraceDebugEnabled;

    // Runtime-tunable palm-rejection thresholds (see AMT_PALM_CONFIG in
    // Public.h). Initialized to AMT_PALM_CONFIG_DEFAULT_INIT in
    // AmtPtpDeviceUsbKmCreateDevice, then optionally overridden from the
    // registry (AmtPalmConfigLoadFromRegistry) and/or live via
    // IOCTL_AMT_PTP_SET_PALM_CONFIG from AmtPtpConfigGui. Read every frame
    // by AmtPalmClassify (Palm.c) - protected by StateLock on write so a
    // config update from the GUI thread can't race a frame in flight.
    AMT_PALM_CONFIG PalmConfig;
    AMT_PALM_RUNTIME PalmRuntime;

    // Runtime-tunable Force Tap (force touch) threshold and click action
    // (see AMT_POINTER_CONFIG in Public.h). Initialized to
    // AMT_POINTER_CONFIG_DEFAULT_INIT in AmtPtpDeviceUsbKmCreateDevice, then
    // optionally overridden from the registry (AmtPointerConfigLoadFromRegistry)
    // and/or live via IOCTL_AMT_PTP_SET_POINTER_CONFIG from AmtPtpConfigGui.
    // Read every frame by PTPCore_ProcessFrame (Ptpcore.c) for the pressure
    // threshold, and by the force-touch delivery block in Interrupt.c for
    // the action - protected by StateLock on write so a config update from
    // the GUI thread can't race a frame in flight.
    AMT_POINTER_CONFIG PointerConfig;
    AMT_POINTER_RUNTIME PointerRuntime;

    // Runtime-tunable two-finger scroll behavior.
    AMT_SCROLL_CONFIG ScrollConfig;
    AMT_SCROLL_RUNTIME ScrollRuntime;

    // TRUE only for trackpads whose packet format actually carries a
    // pressure reading (TYPE4/TYPE5 - see AppleDefinition.h). Derived once
    // from DeviceInfo->tp_type in EvtDevicePrepareHardware. TYPE1-3
    // trackpads (e.g. WELLSPRING8 / pre-Force-Touch MacBook Air) have no
    // pressure channel at all, so force-touch arbitration is meaningless
    // for them - click handling falls back to a plain mechanical hard tap.
    BOOLEAN SupportsForceTouch;

    // Track the prior button state for click-edge detection.
    BOOLEAN PrevButtonClicked;

    // Lock out force-touch if the press drags away from the anchor.
    BOOLEAN ForceTouchAnchorValid;
    USHORT  ForceTouchAnchorX;
    USHORT  ForceTouchAnchorY;
    BOOLEAN ForceTouchDragLockout;

    // MICRO-OPT: precomputed by AmtPointerForceTouchTimingRebuild (called at
    // D0Entry and on every Pointer Config SET/RESET IOCTL) from
    // PointerConfig.ForceTouchEmulationHoldMs and PerfFrequency, so
    // PTPCore_ProcessFrame's hot path never does the ms->ticks division (or
    // touches PointerConfig at all for these three values) on every single
    // frame while a press is pending - see that function for the compare.
    // 0 when there's no usable clock yet, same fallback direction as
    // ClickArbitrationTimeoutTicks below.
    LONGLONG ForceTouchEmulationHoldTicks;

    // Same precomputation, for the two drag-lockout distances
    // (AMT_POINTER_CONFIG::ForceTapDragLockoutDistance /
    // ForceTouchEmulationDragLockoutDistance) - raw sensor units, no
    // conversion needed, just a config->context copy so the hot path has a
    // single place to read from regardless of which of the two paths is
    // active.
    USHORT ForceTapDragLockoutDistanceCached;
    USHORT ForceTouchEmulationDragLockoutDistanceCached;

    // Force-touch click delivery state - see FORCE_TOUCH_DELIVERY_STATE
    // above for the full rationale.
#define PENDING_FORCE_TOUCH_CLICK_CAPACITY 8 // queued (not-yet-started) clicks
    FORCE_TOUCH_DELIVERY_STATE ForceTouchDeliveryState;
    UCHAR                       PendingForceTouchClickCount;

    // Click-arbitration state and timing.
    CLICK_ARBITRATION_STATE ClickArbitrationState;
    USHORT                  ClickArbitrationPeakPressure;
    LONGLONG                ClickArbitrationStartQpc;
    // Frames PeakPressure has NOT grown; lets a flat/light press resolve
    // to HARD_TAP before the full safety-net timeout.
    UCHAR                   ClickArbitrationStallFrames;

    // Scan time
    LARGE_INTEGER LastReportTime;

    // Running scan-time counter for the PTP report.
    ULONG ScanTimeAccumulator;

    // Session-level palm latch used by PTPCore, and where the palm is (the
    // latch is released once no contact remains near this point - see
    // PALM_LATCH_RELEASE_RADIUS in Palm.h).
    BOOLEAN PalmDetected;
    USHORT  PalmAnchorX;
    USHORT  PalmAnchorY;
    // TRUE until a remnant of the palm has been acquired after the last
    // large-palm frame: the anchor is then the palm's centroid, and a
    // peeling-off remnant can sit well away from it (wide capture radius).
    BOOLEAN PalmAnchorFresh;

    // Contact pool for PTPCore and ActiveContact. Allocated separately
    // (not embedded) and manually aligned to 64 bytes in
    // AmtAllocateAlignedContactPool - WDF's context allocator does not
    // honor DECLSPEC_ALIGN, so an embedded array only inherited whatever
    // alignment the pool allocator happened to give the whole context
    // block. Freed in AmtPtpEvtDeviceContextCleanup.
    // ---------------------------------------------------------------
    PACTIVE_CONTACT ActiveContacts;

    // Monotonic ContactID counter - never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry.
    ULONG   NextContactId;

    // Bounded, escalating recovery of the interrupt pipe's continuous
    // reader after AmtPtpEvtUsbInterruptReadersFailed. Without this, a
    // pipe that keeps failing right after a resume (endpoint not yet
    // settled) gets resubmitted in a tight loop with no delay - real CPU
    // and USB-controller cost, not just a cosmetic retry. Reset in
    // AmtPtpEvtDeviceD0Entry and on every successful read completion.
    //
    // BUG FIX (was: "cancelled synchronously in AmtPtpEvtDeviceD0Exit
    // before the pipe's I/O target is stopped, so it can never fire
    // against a pipe that is concurrently being torn down" - that
    // description stopped being true when D0Exit's WdfTimerStop call was
    // changed to Wait=FALSE to avoid Bug Check 0x9F on a stuck recovery
    // call; nobody updated this comment or added the synchronization the
    // old comment assumed). AmtPtpEvtReaderRestartTimer (Interrupt.c) can
    // now genuinely still be executing - including inside a blocking
    // WdfUsbTargetPipeResetSynchronously/ResetPortSynchronously call - at
    // the exact moment AmtPtpEvtDeviceD0Exit runs. Confirmed as the cause
    // of a real Bug Check 0x10D (WDF_VIOLATION, Parameter1=0x5 "handle of
    // incorrect type", Parameter2=0x0) during a sleep transition: with S3
    // known to genuinely disconnect this device (see the
    // WdfUsbTargetDeviceIsConnectedSynchronous comment below), InterruptPipe
    // can go NULL (AmtPtpEvtDeviceReleaseHardware) while one of these two
    // racing call paths is mid-read of it, producing a NULL handle passed
    // into WdfUsbTargetPipeGetIoTarget. D0ExitInProgress below closes that
    // window without reintroducing the 0x9F risk: D0Exit sets it (Interlocked,
    // no blocking) before doing anything else, and the timer callback
    // checks it - and NULL-guards InterruptPipe itself - before every use.
    WDFTIMER               ReaderRestartTimer;

    // Lifecycle-protected - every read/write goes through D0ExitLock (see
    // below). Never left as a plain concurrent-access variable: it is
    // read/written from both the PASSIVE_LEVEL D0Entry/D0Exit/
    // ReleaseHardware/timer paths and the DISPATCH_LEVEL
    // AmtPtpEvtUsbInterruptReadersFailed path.
    READER_RECOVERY_STAGE  ReaderRecoveryStage;

    // TWO-LEVEL SYNCHRONIZATION MODEL
    // ---------------------------------------------------------------
    // D0ExitLock (WDFSPINLOCK) and RecoveryLock (WDFWAITLOCK) below serve
    // deliberately different, non-overlapping purposes - conflating them
    // into one primitive is what earlier revisions of this driver got
    // wrong, in two different directions (a bare Interlocked flag that
    // wasn't atomic as a check-then-read sequence; then a single spinlock
    // that either had to be dropped around every blocking USB call, losing
    // its exclusion guarantee, or held across them, violating IRQL rules).
    //
    //   D0ExitLock  - DISPATCH_LEVEL-safe. Guards only short, non-blocking
    //                  state transitions: D0ExitInProgress,
    //                  ReaderRecoveryStage, RecoveryGeneration, and
    //                  snapshot reads of InterruptPipe/UsbDevice/
    //                  UsbInterface. Never held across a blocking WDF/USB
    //                  call.
    //   RecoveryLock - PASSIVE_LEVEL-only. Serializes the actual recovery
    //                  lifecycle work: reader recovery (the
    //                  READER_RECOVERY_STAGE ladder), D0Exit cleanup,
    //                  ReleaseHardware cleanup, and the deferred
    //                  AmtPtpSetWellspringMode(TRUE) work item queued by
    //                  D0Entry and reader/PnP recovery paths. Guarantees
    //                  at most one of { WdfIoTargetStop,
    //                  WdfUsbTargetPipeResetSynchronously,
    //                  WdfUsbTargetDeviceResetPortSynchronously,
    //                  AmtPtpCyclePort, WdfIoTargetStart,
    //                  AmtPtpSetWellspringMode } sequence is ever in flight
    //                  at a time. Never acquired from DISPATCH_LEVEL.
    //
    // A caller that needs both takes D0ExitLock first for a quick
    // check-and-snapshot, releases it, then acquires RecoveryLock for the
    // actual (possibly blocking) work, then re-validates its snapshot under
    // D0ExitLock once more before touching anything - see
    // AmtPtpEvtReaderRestartTimer for the canonical shape. D0ExitLock is
    // never held while waiting on RecoveryLock and never held across any
    // blocking call - only across the handful of instructions needed for a
    // state transition or a pointer snapshot.
    WDFWAITLOCK               RecoveryLock;

    // ---------------------------------------------------------------
    // GLOBAL LOCK ACQUISITION ORDER (audited across ConfigIoctl.c,
    // Device.c, Hid.c, Interrupt.c)
    // ---------------------------------------------------------------
    // This driver uses five locks total: RecoveryLock, D0ExitLock,
    // StateLock, LiveLock (all above/below in this struct), and
    // DRIVER_CONTEXT::ConfigControlDeviceLock (Driver.h, driver-lifetime,
    // not per-FDO).
    //
    //   1. RecoveryLock          (WDFWAITLOCK, PASSIVE_LEVEL only)
    //   2. D0ExitLock            (WDFSPINLOCK, DISPATCH_LEVEL-safe)
    //
    // RecoveryLock, when taken together with D0ExitLock, is ALWAYS
    // acquired first, with D0ExitLock nested inside it for a short
    // snapshot/validate (see AmtPtpEvtReaderRestartTimer for the canonical
    // shape) - never the reverse. D0ExitLock is never held while waiting
    // on RecoveryLock and never held across a blocking call.
    //
    // StateLock, LiveLock, and ConfigControlDeviceLock are independent
    // leaf locks: no call site acquires any of them while already holding
    // RecoveryLock, D0ExitLock, or each other. Each guards its own
    // self-contained critical section (frame-processing state,
    // live-monitor snapshot publication, and the control device's
    // TargetDevice field, respectively) and is released before any other
    // lock in this list is taken.
    //
    // Net rule: the only real ordering constraint is RecoveryLock before
    // D0ExitLock. StateLock/LiveLock/ConfigControlDeviceLock can be taken
    // at any point as long as they are not held across a call into another
    // lock's critical section. Keep it that way - do not acquire two of
    // these five locks in a new nested combination without updating this
    // comment to reflect the resulting order.
    // ---------------------------------------------------------------

    // Set under D0ExitLock at the very start of AmtPtpEvtDeviceD0Exit and
    // AmtPtpEvtDeviceReleaseHardware, cleared under the same lock at the
    // start of AmtPtpEvtDeviceD0Entry. AmtPtpEvtReaderRestartTimer and
    // AmtPtpEvtUsbInterruptReadersFailed both take D0ExitLock, check this
    // flag, and - in the same critical section - snapshot InterruptPipe/
    // UsbDevice/RecoveryGeneration into locals before releasing the lock,
    // so a caller can never observe this flag clear and then read a
    // pipe/device handle that D0Exit or AmtPtpEvtDeviceReleaseHardware
    // changed a moment later. This BOOLEAN is only ever touched with
    // D0ExitLock held - no Interlocked/volatile needed once every access
    // goes through the lock.
    BOOLEAN                  D0ExitInProgress;

    // T2 resume lifecycle gates. T2ResumeActive is set before WdfIoTargetStart
    // for a normal D3 -> D0 transition so an early ReadersFailed callback
    // can identify the resume session even if the callback fires from inside
    // WdfIoTargetStart. T2RestartPending is latched when that failure schedules
    // the required PnP re-enumeration and is cleared by the next D0Entry/D0Exit
    // lifecycle transition. Both are protected by D0ExitLock.
    BOOLEAN                  T2ResumeActive;
    BOOLEAN                  T2RestartPending;

    // Lifecycle generation token. Incremented under D0ExitLock at the start
    // of every new D0Entry/D0Exit/ReleaseHardware transition. A recovery
    // path (timer callback) that snapshots this value before dropping
    // D0ExitLock, then re-checks it after acquiring RecoveryLock and
    // re-taking D0ExitLock, can detect that lifecycle has moved on in the
    // interim (e.g. a stale queued timer callback from a D0 session that
    // has already exited and re-entered) and bail out instead of operating
    // on state that belongs to a different generation. A bare
    // D0ExitInProgress flag alone cannot express this: it only says
    // "lifecycle is currently transitioning", not "this is/isn't the same
    // lifecycle session the snapshot was taken from".
    ULONG                     RecoveryGeneration;

    // Guards D0ExitInProgress, ReaderRecoveryStage, and RecoveryGeneration
    // above, together with consistent snapshot reads of InterruptPipe/
    // UsbDevice/UsbInterface. WDFSPINLOCK, not WDFWAITLOCK, because
    // AmtPtpEvtUsbInterruptReadersFailed can run at up to DISPATCH_LEVEL
    // (it fires from the same completion path as EvtUsbTargetPipeReadComplete,
    // which the WDK documents as "typically DISPATCH_LEVEL, but no higher")
    // and WDFWAITLOCK only supports PASSIVE_LEVEL acquisition. Held only
    // across flag set/check and pointer-snapshot instructions themselves -
    // never across WdfIoTargetStop/WdfUsbTargetPipeResetSynchronously/
    // WdfUsbTargetDeviceResetPortSynchronously/AmtPtpCyclePort/
    // WdfWaitLockAcquire, all of which are PASSIVE_LEVEL-only or blocking
    // calls that would violate IRQL rules (or defeat the whole point of
    // avoiding WdfTimerStop's Wait=TRUE) if issued while a spinlock is
    // held.
    WDFSPINLOCK              D0ExitLock;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // MICRO-OPT: PerfFrequency-derived tick thresholds, computed once at
    // D0Entry instead of on every call site that needs them (retap window
    // check on every birth, match time-reject on every accepted match,
    // click-arbitration timeout on every pending frame). Each of those was
    // a 64-bit multiply+divide recomputed from the same constant frequency
    // every time; now it's a plain field read. 0 means "no usable clock"
    // (PerfFrequency.QuadPart <= 0), preserving the original fail-closed/
    // fail-open behavior of each call site exactly.
    LONGLONG      VelocityWindowTicks;           // velocity history cap (AMT_VELOCITY_WINDOW_MS)
    LONGLONG      RetapWindowTicks;             // AmtRecentLiftFindNearby window
    LONGLONG      MatchMaxTimeDeltaTicks;       // AmtMatchCorrespond time-reject
    LONGLONG      ClickArbitrationTimeoutTicks; // click arbitration safety-net
    LONGLONG      GestureSettleTicks;           // AMT_GESTURE_SETTLE_MS landing-settle window

    // QPC deadline until which emitted multi-finger motion is deferred after
    // a finger landed next to another contact (see AMT_GESTURE_SETTLE_MS in
    // Activecontact.h). A deadline in the past (or 0) means no hold.
    LONGLONG      GestureSettleUntilQpc;

    // TRUE while a 2-contact touch is in scroll motion mode (both fingers
    // moved). Latched so the mode doesn't flicker frame to frame; cleared
    // as soon as fewer than 2 contacts are down. See
    // AMT_SCROLL_MODE_MOVE_MIN_UNITS in Activecontact.h.
    BOOLEAN       ScrollModeLatched;

    // MICRO-OPT: Q16 fixed-point form of (10000 / PerfFrequency), computed
    // once at D0Entry instead of a 64-bit divide (and a PerfFrequency>0
    // branch) on every single USB interrupt completion (the hottest
    // routine in the driver - see AmtPtpEvtUsbInterruptPipeReadComplete).
    // Runtime becomes an unconditional multiply + shift:
    // (PerfDelta * ScanTimeScaleQ16) >> 16, same result as the old
    // "* 10000LL / PerfFrequency.QuadPart" to within the ScanTime field's
    // USHORT-truncated precision (rounding differs by at most 1 part in
    // 65536, invisible after the field's own 0xFFFF wraparound). Q16 (not
    // Q32) is deliberate: it keeps PerfDelta*ScanTimeScaleQ16 far from
    // LONGLONG overflow even after a multi-day idle gap between reports,
    // where Q32's much larger scale factor would leave far less headroom.
    // When there's no usable clock (PerfFrequency.QuadPart <= 0, meaning
    // KeQueryPerformanceCounter reported a degenerate frequency - not
    // expected on real hardware), this is set to 655 instead of 0: solving
    // (x*scale)>>16 == x/100 for scale gives 65536/100 = 655.36, matching
    // the old "/100LL" fallback formula while still avoiding a branch on
    // the hot path - the fallback is already a best-effort approximation,
    // so the ~0.05% rounding difference from truncating to 655 is moot.
    LONGLONG      ScanTimeScaleQ16;

    // Deferred overflow queue for contact events.
    // ---------------------------------------------------------------
    ULONG         OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowY[PTP_MAX_CONTACT_POINTS];
    CONTACT_PHASE OverflowPhase[PTP_MAX_CONTACT_POINTS];
    BOOLEAN       OverflowConfident[PTP_MAX_CONTACT_POINTS];
    UCHAR         OverflowCount;

    // Recent-lift memory for retap smoothing.
    // ---------------------------------------------------------------
    RECENT_LIFT_RING RecentLifts;

    // ---------------------------------------------------------------
    // Cold: USB setup metadata. Written once (EvtDevicePrepareHardware /
    // device creation), never read in the per-frame hot path (grep across
    // Interrupt.c/Hid.c/Ptpcore.c/Match.c confirms zero hits) - kept at the
    // end of the struct so it doesn't sit between hot fields and cost extra
    // cache-line fetches on the frame-processing path.
    // ---------------------------------------------------------------
    WDFUSBINTERFACE        UsbInterface;
    USB_DEVICE_DESCRIPTOR  DeviceDescriptor;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// Bound the Wellspring control transfer with a timeout.
#define WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC   5

// Bound RecoveryLock acquisition in ReleaseHardware/D0Entry/D0Exit. WDF's
// own WdfUsbTargetPipeResetSynchronously / WdfUsbTargetDeviceResetPortSynchronously
// have no timeout (see the Bug Check 0x9F comment in AmtPtpEvtDeviceD0Exit),
// so AmtPtpEvtReaderRestartTimer can hold RecoveryLock indefinitely if the
// USB stack's own request build/completion for one of those calls wedges
// (e.g. a Driver Verifier Low Resources Simulation injection landing
// inside it). D0Exit is covered by the power manager's watchdog in that
// case (worst case: bugcheck 0x9F, at least a dump). ReleaseHardware and
// D0Entry are not - an infinite WdfWaitLockAcquire there hangs the whole
// device stack with no bugcheck and no minidump. See
// AmtPtpRecoveryLockAcquireBounded in Device.c.
#define RECOVERY_LOCK_ACQUIRE_TIMEOUT_MS 5000

// Small settle delay before each escalation step in the reader-recovery
// ladder (READER_RECOVERY_STAGE above). The recovery calls themselves
// (port reset, port cycle) already take real time on the wire; this just
// avoids hammering an endpoint that is mid-transition between one
// escalation step and the next.
#define READER_RECOVERY_STEP_DELAY_MS 100

// Bounded retries for the Wellspring mode-switch control transfer after a
// full power-off (D3Final -> D0). On the initial/full enumeration path the
// retry loop runs synchronously in D0Entry after WdfIoTargetStart succeeds;
// on normal D3 -> D0 resume the same budget is consumed by the deferred
// the PASSIVE_LEVEL T2 PnP recovery timer.
#define WELLSPRING_MODE_D0ENTRY_MAX_ATTEMPTS       3
#define WELLSPRING_MODE_D0ENTRY_RETRY_DELAY_MS_UNIT 50

// A normal D3 -> D0 resume can reach D0Entry before the USB child has
// finished re-enumerating on the parent hub. Per Microsoft's own guidance
// for this exact fault class - "Low Resources Simulation"
// (learn.microsoft.com/windows-hardware/drivers/devtest/low-resources-simulation):
// "Driver Verifier fails random instances of the driver's memory
// allocations [...] This tests the driver's ability to respond properly to
// low memory and other low-resource conditions" - the canonical response to
// an injected allocation failure is not just to fail the one call cleanly,
// but to retry it, the same as any other transient condition. Confirmed
// against SAKURAMBPRO.log (Driver Verifier "Pool Allocations Failed
// Deliberately: 1"): at t=297s a resume's SetWellspringMode/WdfIoTargetStart
// never got a chance to succeed within the old 2-attempt/50ms budget, and
// the device stayed torn down for ~45s (through an entire extra sleep/wake
// cycle) before recovering on its own. In this path Device.c now retries
// both STATUS_NO_SUCH_DEVICE (the child not back on the bus yet) and
// STATUS_INSUFFICIENT_RESOURCES (a Verifier-injected or genuine low-memory
// failure of the WDFREQUEST/WDFMEMORY allocation inside
// WdfUsbTargetDeviceSendControlTransferSynchronously) - both are transient
// and equally recoverable by retry; all other failures remain immediate
// failures. Budget widened from 2/50ms to 5/75ms-per-step (linear backoff,
// worst case ~5*75=375ms extra) to give either condition real room to clear.

// Bounded retries for the D0Entry interrupt-pipe WdfIoTargetStart, after
// the same D3Final -> D0 transition. This is subject to the identical
// "device not accepting requests yet" window as SetWellspringMode above,
// but unlike that best-effort control transfer, WdfIoTargetStart's result
// IS this driver's EvtDeviceD0Entry return value: per the WDF "Reporting
// Device Failures" documentation, a callback that reports !NT_SUCCESS
// causes the framework to ask the bus driver to reenumerate the device
// (full FDO teardown/recreate), and after a few consecutive such failures
// the framework stops attempting to restart it at all. A lost race here
// is therefore far more disruptive than a lost race on the Wellspring
// transfer, and giving one of the two a retry while leaving the other bare
// is a bug, not a style choice - both go through the same shared retry
// helper (AmtPtpD0EntryRetry in Device.c).
#define INTERRUPT_PIPE_D0ENTRY_MAX_ATTEMPTS        3
#define INTERRUPT_PIPE_D0ENTRY_RETRY_DELAY_MS_UNIT 50

// Small, separate retry budget for the D3 -> D0 interrupt-target start.
// This retry remains intentionally in place for Driver Verifier / low-resource
// testing: STATUS_INSUFFICIENT_RESOURCES must not make one injected allocation
// failure fail the power transition unnecessarily. A T2 reader failure after
// a successful start is handled by PnP re-enumeration rather than by retrying
// Wellspring in place.
// WdfIoTargetStart
// itself does not allocate per Microsoft Learn's own documented return
// codes (learn.microsoft.com/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetstart),
// but the continuous reader it starts pre-allocates its WDFREQUEST pool via
// WdfUsbTargetPipeConfigContinuousReader back in EvtDevicePrepareHardware,
// which Microsoft Learn documents as returning STATUS_INSUFFICIENT_RESOURCES
// on exactly this kind of injected/genuine low-memory condition
// (learn.microsoft.com/windows-hardware/drivers/ddi/wdfusb/nf-wdfusb-wdfusbtargetpipeconfigcontinuousreader) -
// that path is already retried in EvtDevicePrepareHardware itself (see
// AmtPtpPrepareHardwareRetryOnLowResources in Device.c); this budget only
// covers the resume-time race on WdfIoTargetStart re-arming that
// already-allocated reader.
#define INTERRUPT_PIPE_D0ENTRY_RESUME_MAX_ATTEMPTS        5
#define INTERRUPT_PIPE_D0ENTRY_RESUME_RETRY_DELAY_MS_UNIT 75

// Bounded retries for the IOCTL_HID_SET_FEATURE / REPORTID_REPORTMODE path
// in AmtPtpSetFeatures (Hid.c), i.e. Windows' own multitouch input-mode
// configuration step (source "MTConfig" in the System event log) rather
// than our D0Entry path. Same underlying race - the device may still be
// settling from a USB resume when this SET_FEATURE lands - so it gets the
// same bounded retry-with-backoff shape as WELLSPRING_MODE_D0ENTRY_* above.
#define WELLSPRING_MODE_SETFEATURE_MAX_ATTEMPTS        3
#define WELLSPRING_MODE_SETFEATURE_RETRY_DELAY_MS_UNIT 50

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

// AmtPtpRetryOnLowResourcesCtx - shared bounded-retry engine (see the
// definition in Device.c, next to AmtPtpRetryOnLowResources, for the full
// rationale): 3 attempts, STATUS_INSUFFICIENT_RESOURCES only, increasing
// backoff. Context-pointer-based so a single engine covers attempts that
// need more than a bare WDFDEVICE - currently AmtPtpAcquireConfigControlDevice's
// control-device WdfDeviceCreate/WdfDeviceCreateSymbolicLink/WdfIoQueueCreate
// (Device.c) and both WdfIoQueueCreate calls in AmtPtpDeviceUsbKmQueueInitialize
// (Queue.c). TraceContext may be NULL (AmtTrace() tolerates that).
typedef NTSTATUS
(*PFN_AMT_LOW_RESOURCES_ATTEMPT_CTX)(_Inout_ PVOID Context);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpRetryOnLowResourcesCtx(
    _In_opt_ PDEVICE_CONTEXT                  TraceContext,
    _In_     PCSTR                             OperationName,
    _In_     PFN_AMT_LOW_RESOURCES_ATTEMPT_CTX Attempt,
    _Inout_  PVOID                             Context
    );

// AMT_QUEUE_CREATE_CTX / AmtPtpQueueCreateAttempt - the one
// AmtPtpRetryOnLowResourcesCtx attempt/context pair every WdfIoQueueCreate
// call site in this driver shares: AmtPtpAcquireConfigControlDevice's
// control-device queue (Device.c) and both queues in
// AmtPtpDeviceUsbKmQueueInitialize (Queue.c). OutQueue may be
// WDF_NO_HANDLE, same as a direct WdfIoQueueCreate call.
typedef struct _AMT_QUEUE_CREATE_CTX {
    WDFDEVICE             Device;
    PWDF_IO_QUEUE_CONFIG  QueueConfig;
    WDFQUEUE*             OutQueue;
} AMT_QUEUE_CREATE_CTX, *PAMT_QUEUE_CREATE_CTX;

NTSTATUS
AmtPtpQueueCreateAttempt(_Inout_ PVOID Context);

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE AmtPtpEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP  AmtPtpEvtDeviceContextCleanup;

// The PnP device is a lower filter. User-mode configuration therefore uses
// a separate KMDF control device with a DOS symbolic link instead of a
// device interface attached to the USB/HID filter FDO.
//
// The control device is driver-lifetime, not FDO-lifetime: the first call
// (from any FDO's EvtDeviceAdd) creates it under DRIVER_CONTEXT and every
// later call just re-points its TargetDevice at the calling FDO. See the
// comment on DRIVER_CONTEXT (Driver.h) and on this function's definition
// (Device.c) for why - in short, a per-FDO control device with a fixed
// name cannot survive the surprise-removal/re-enumeration that this
// device's parent USB hub performs on nearly every sleep/wake.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpAcquireConfigControlDevice(_In_ WDFDEVICE TargetDevice);

// AmtPtpConfigControlSnapshotTargetDevice / AmtPtpConfigControlReleaseTargetDevice
//
// The only safe way to read AMT_CONFIG_CONTROL_CONTEXT::TargetDevice and
// use the FDO it names.
//
// Every WRITE to that field (AmtPtpAcquireConfigControlDevice's re-attach
// branch, AmtPtpEvtDeviceContextCleanup's NULL-out) already runs under
// DRIVER_CONTEXT::ConfigControlDeviceLock - a WDFWAITLOCK, so PASSIVE_LEVEL
// only, but otherwise cheap and never held across a blocking call. Every
// IOCTL handler used to read the field directly with no lock at all
// (ConfigIoctl.c), which is exactly the same class of bug the
// D0ExitLock/RecoveryLock two-phase pattern elsewhere in this driver
// exists to prevent: a sleep-triggered surprise-removal/re-enumeration can
// null the field out (or re-point it at a brand-new FDO) between an
// unlocked check and an unlocked use a few lines later, with
// AmtPtpGetLiveFrame's separate check-then-dereference (called at up to
// 30 Hz whenever the GUI's Live preview is on) the worst offender - the
// two reads are not even the same snapshot, let alone a locked one.
//
// BUG FIX (use-after-free): reading the field under lock closes the TOCTOU
// on the *pointer value*, but a lock-protected read is not a reference. A
// bare WDFDEVICE sitting in a local variable does not keep that FDO's
// framework object alive. If the FDO is torn down (surprise removal ->
// ReleaseHardware -> AmtPtpEvtDeviceContextCleanup -> WDF frees the object)
// while a handler is still mid-flight against a previously-snapshotted
// handle - trivially reachable, since several handlers (AmtPtpSetPalmConfig
// et al.) do blocking PASSIVE_LEVEL registry I/O in between - that handler's
// subsequent DeviceGetContext(targetDevice) dereferences freed pool. The
// missing piece is exactly what the AMT_CONFIG_CONTROL_CONTEXT field
// comment used to (incorrectly) claim already existed: an explicit
// reference pinning the FDO alive for the duration of use.
//
// AmtPtpConfigControlSnapshotTargetDevice now takes a WdfObjectReference on
// the returned handle before releasing ConfigControlDeviceLock (so the
// reference-take itself cannot race the field going NULL/repointed) and
// returns NULL if there is no current target. Every caller MUST release
// that reference - exactly once, on every exit path, including early
// returns - via AmtPtpConfigControlReleaseTargetDevice() once it is done
// touching the target FDO's context. This does not prevent the FDO from
// being logically removed (D0ExitInProgress/RecoveryLock still govern
// that) - it only guarantees the WDFDEVICE handle and its DEVICE_CONTEXT
// memory remain valid to dereference for as long as the caller holds the
// reference.
_IRQL_requires_(PASSIVE_LEVEL)
WDFDEVICE
AmtPtpConfigControlSnapshotTargetDevice(_In_ WDFDEVICE ControlDevice);

// Releases the reference taken by AmtPtpConfigControlSnapshotTargetDevice.
// No-op if TargetDevice is NULL (mirrors callers' existing NULL-check
// pattern, so call sites don't need a separate guard). Safe at
// PASSIVE_LEVEL <= IRQL <= DISPATCH_LEVEL, matching WdfObjectDereference's
// own contract, though every current caller is PASSIVE_LEVEL.
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPtpConfigControlReleaseTargetDevice(_In_opt_ WDFDEVICE TargetDevice);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtPtpConfigControlEvtIoDeviceControl;

// Fires when the last handle to \\DosDevices\\AmtPtpDeviceUsbKm closes -
// including an abnormal GUI exit (crash/kill/unplug), where the GUI never
// gets a chance to run its own Closed handler. Used as a safety net to
// force LiveEnabled back off so the interrupt hot path never keeps building
// live snapshots for a monitor that no longer exists.
EVT_WDF_FILE_CLOSE AmtPtpConfigControlEvtFileClose;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpConfigControlEvtConfigControlCleanup;

NTSTATUS
AmtPtpSetLiveEnabled(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS
AmtPtpGetLiveFrame(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(_In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext);

BOOLEAN
AmtPtpIsT2Device(_In_ const PDEVICE_CONTEXT DeviceContext);

EVT_WDF_USB_READER_COMPLETION_ROUTINE AmtPtpEvtUsbInterruptPipeReadComplete;
EVT_WDF_USB_READERS_FAILED            AmtPtpEvtUsbInterruptReadersFailed;
EVT_WDF_TIMER                         AmtPtpEvtReaderRestartTimer;

// WdfIoQueueReadyNotify callback for InputQueue (registered in
// AmtPtpDeviceUsbKmQueueInitialize, Queue.c). Closes the gap where a
// pending force-touch mouse click (ForceTouchDeliveryState != IDLE) has
// no touch interrupt left to retry delivery on - see the comment above
// AmtPtpTryDeliverForceTouchClick in Interrupt.c for the full rationale.
EVT_WDF_IO_QUEUE_STATE AmtPtpEvtInputQueueReady;

// Last rung of the reader-recovery escalation ladder - see
// READER_RECOVERY_STAGE. Power-cycles the device's USB port, the moral
// equivalent of an unplug/replug, via IOCTL_INTERNAL_USB_CYCLE_PORT sent to
// the WDFUSBDEVICE's I/O target - deliberately the raw IOCTL rather than
// the WdfUsbTargetDeviceCyclePortSynchronously wrapper, which has no
// time-out parameter (see the comment above AmtPtpCyclePort's definition
// in Interrupt.c). Must be called at PASSIVE_LEVEL.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCyclePort(_In_ PDEVICE_CONTEXT DeviceContext);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN IsWellspringModeOn
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetHidDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetDeviceAttribs(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetReportDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS AmtPtpDispatchReadReportRequests(
    _In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _Out_ BOOLEAN* Pending);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpReportFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpSetFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

// ConfigIoctl.c - AmtPtpConfigGui <-> driver custom IOCTL surface.

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPadGeometry(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetDeviceInfo(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

// Best-effort registry persistence under the device's driver-software key
// (HKLM\SYSTEM\...\Enum\...\Device Parameters, via WdfDeviceOpenRegistryKey).
// Failure to read/write the registry is never fatal - PalmConfig always
// falls back to AMT_PALM_CONFIG_DEFAULT_INIT.
_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtPalmConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_PALM_CONFIG Config);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID AmtPalmConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_PALM_CONFIG* Config);

// Clamps every tunable field of Config in place to the ranges declared in
// Public.h (AMT_PALM_*_MAX). Used on every path that accepts values from
// user mode (SET IOCTL and registry load) so a corrupt registry value or a
// buggy/malicious caller can never push the classifier into a degenerate
// state (e.g. an edge zone covering the whole pad).
VOID AmtPalmConfigClamp(_Inout_ PAMT_PALM_CONFIG Config);

VOID AmtPalmRuntimeRebuild(
    _In_ const AMT_PALM_CONFIG* Config,
    _Out_ AMT_PALM_RUNTIME* Runtime
);

// Same best-effort registry persistence and clamp contract as the
// AmtPalmConfig* trio above, for AMT_POINTER_CONFIG.
_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtPointerConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_POINTER_CONFIG Config);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID AmtPointerConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_POINTER_CONFIG* Config);

VOID AmtPointerConfigClamp(_Inout_ PAMT_POINTER_CONFIG Config);

_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtScrollConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_SCROLL_CONFIG Config);

VOID AmtScrollConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_SCROLL_CONFIG* Config);
VOID AmtScrollConfigClamp(_Inout_ PAMT_SCROLL_CONFIG Config);

VOID AmtPointerRuntimeRebuild(
    _In_ const AMT_POINTER_CONFIG* Config,
    _Out_ AMT_POINTER_RUNTIME* Runtime
);

// Precomputes the Force Touch hold-timer threshold (ms -> QPC ticks) and
// copies out the two drag-lockout distances, all from
// DeviceContext->PointerConfig + DeviceContext->PerfFrequency, into the
// DEVICE_CONTEXT fields PTPCore_ProcessFrame's hot path reads directly (see
// those fields' comments). Call after PerfFrequency and/or PointerConfig
// change: D0Entry (clock becomes known) and every SET/RESET Pointer Config
// IOCTL (value becomes known) - see call sites in Device.c/ConfigIoctl.c.
// Safe to call before D0Entry too: PerfFrequency.QuadPart reads 0 then, so
// ForceTouchEmulationHoldTicks lands on the same "never resolves early"
// fallback D0Entry's own tick-precompute block already documents.
VOID AmtPointerForceTouchTimingRebuild(_Inout_ PDEVICE_CONTEXT DeviceContext);

VOID AmtScrollRuntimeRebuild(
    _In_ const AMT_SCROLL_CONFIG* Config,
    _Out_ AMT_SCROLL_RUNTIME* Runtime
);

// ---------------------------------------------------------------------
// RECOVERY/LIFECYCLE D0ExitLock HELPERS
// ---------------------------------------------------------------------
// Pure extraction of the repeated D0ExitLock critical sections used by
// AmtPtpEvtDeviceD0Exit, AmtPtpEvtDeviceReleaseHardware,
// AmtPtpEvtDeviceD0Entry, AmtPtpEvtUsbInterruptPipeReadComplete,
// AmtPtpEvtUsbInterruptReadersFailed, and AmtPtpEvtReaderRestartTimer (see
// the two-level synchronization model comment above D0ExitInProgress in
// this header). Each helper covers exactly one identical-shaped critical
// section that appeared 2+ times verbatim - it changes no locking order,
// no IRQL contract, and no field semantics versus the code it replaces.
// Sites whose D0ExitLock section does something *different* from its
// neighbors (e.g. the stage-refresh-plus-D0ExitInProgress-check in
// AmtPtpEvtReaderRestartTimer's Phase 1/Phase 2, or the stage-advance
// later in the same function) are deliberately left inline rather than
// forced into one of these, to keep each helper's contract exact and
// auditable. A third helper (a generic D0ExitInProgress+stage+generation
// snapshot) was considered and dropped: only one call site (in
// AmtPtpEvtUsbInterruptReadersFailed) actually matches its shape - the
// other candidate (AmtPtpEvtReaderRestartTimer's Phase 1) also snapshots
// InterruptPipe and has an inline early-return, so wrapping it would have
// meant distorting that site to fit the helper instead of the other way
// around.
//
// Both are DISPATCH_LEVEL-safe (D0ExitLock is a WDFSPINLOCK) and never
// block.

// Common "Step 1" of AmtPtpEvtDeviceD0Exit and
// AmtPtpEvtDeviceReleaseHardware: mark the lifecycle as terminating and
// bump the generation token, in one D0ExitLock critical section, before
// either function does anything else (WdfTimerStop, RecoveryLock, etc.).
// Callers are unchanged otherwise - this only replaces the 4-line
// Acquire/set/set/Release block that was identical in both places.
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID AmtPtpRecoveryBeginTermination(_In_ PDEVICE_CONTEXT DeviceContext);

// If RecoveryGeneration still equals SnapshotGeneration (i.e. no
// D0Exit/D0Entry/ReleaseHardware has started since the caller's snapshot),
// sets ReaderRecoveryStage to READER_RECOVERY_EXHAUSTED. Returns TRUE if
// the generation matched (state was updated), FALSE if the snapshot was
// stale (nothing was touched - a newer lifecycle session owns the field
// now). Replaces the identical
// "Acquire/if(generation==snapshot){stage=EXHAUSTED}/Release" block used
// at three call sites in AmtPtpEvtReaderRestartTimer.
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN AmtPtpRecoveryMarkExhaustedIfCurrent(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ ULONG           SnapshotGeneration);

// WdfWaitLockAcquire(RecoveryLock, NULL) blocks forever. RecoveryLock is
// held by AmtPtpEvtReaderRestartTimer (Interrupt.c) across
// WdfUsbTargetPipeResetSynchronously / WdfUsbTargetDeviceResetPortSynchronously
// - both documented as having NO timeout - so any *other* acquirer of
// RecoveryLock (AmtPtpEvtDeviceReleaseHardware / AmtPtpEvtDeviceD0Entry /
// AmtPtpEvtDeviceD0Exit in Device.c, and
// AmtPtpSetFeatures's REPORTID_REPORTMODE/SetWellspringMode path in Hid.c)
// must use this bounded wrapper instead of acquiring RecoveryLock directly
// - an infinite wait on any one of them hangs that call path with no
// bugcheck and no minidump if the timer is ever stuck. Returns TRUE with
// the lock held, or FALSE (lock NOT held) after RECOVERY_LOCK_ACQUIRE_TIMEOUT_MS.
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN AmtPtpRecoveryLockAcquireBounded(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ PCSTR           CallerName);

EXTERN_C_END