// Route USB input through PTPCore into HID reports.

#include "Driver.h"
#include "PTPCore.h"
#include "Input.h"

// Forward declaration: defined below AmtPtpEvtUsbInterruptPipeReadComplete,
// called from it and from AmtPtpEvtInputQueueReady (also this file).
static VOID
AmtPtpTryDeliverForceTouchClick(_In_ PDEVICE_CONTEXT pCtx);

static VOID
AmtPtpTryDeliverPendingTouchReport(_In_ PDEVICE_CONTEXT pCtx);

BOOLEAN
AmtPtpIsT2Device(_In_ const PDEVICE_CONTEXT DeviceContext)
{
    if (DeviceContext->DeviceInfo == NULL) {
        return FALSE;
    }

    switch ((USHORT)DeviceContext->DeviceInfo->identification) {
    case USB_DEVICE_ID_APPLE_T2_J152F:
    case USB_DEVICE_ID_APPLE_T2_J680:
    case USB_DEVICE_ID_APPLE_T2_J140K:
    case USB_DEVICE_ID_APPLE_T2_J132:
    case USB_DEVICE_ID_APPLE_T2_J213:
    case USB_DEVICE_ID_APPLE_T2_J214K:
    case USB_DEVICE_ID_APPLE_T2_J223:
    case USB_DEVICE_ID_APPLE_T2_J230K:
        return TRUE;
    default:
        return FALSE;
    }
}

#ifdef ALLOC_PRAGMA
// AmtPtpCyclePort is the only PASSIVE_LEVEL-only, non-hot-path function in
// this file that stays genuinely pageable - everything else here runs on
// the USB completion/DPC path and must stay non-paged.
//
// AmtPtpEvtReaderRestartTimer is deliberately NOT listed here (and no
// longer calls PAGED_CODE() either - see the comment at its definition):
// although it is PASSIVE_LEVEL-only (the timer was created with
// ExecutionLevel = WdfExecutionLevelPassive specifically for this), it
// also acquires D0ExitLock (a WDFSPINLOCK) as part of the two-level
// synchronization model in Device.h, which briefly raises IRQL to
// DISPATCH_LEVEL. Code in the PAGE section must never run above
// APC_LEVEL - a fault while paged-out code is executing at raised IRQL
// bugchecks - so alloc_text(PAGE, ...) on a spinlock-acquiring function is
// a genuine correctness bug (caught here by PREfast as C28150), not just
// style. Keeping this callback resident costs a small, fixed amount of
// nonpaged memory in exchange for that guarantee.
#pragma alloc_text (PAGE, AmtPtpCyclePort)
#endif

#if DBG
static VOID
AmtReportCheckInvariants(_In_ const PTP_REPORT* Report)
{
    NT_ASSERT(Report->ContactCount <= PTP_MAX_CONTACT_POINTS);

    for (UCHAR a = 0; a < Report->ContactCount; a++) {
        NT_ASSERT(Report->Contacts[a].TipSwitch == 0 || Report->Contacts[a].TipSwitch == 1);
        for (UCHAR b = (UCHAR)(a + 1); b < Report->ContactCount; b++) {
            NT_ASSERT(Report->Contacts[a].ContactID != Report->Contacts[b].ContactID);
        }
    }
}
#else
#define AmtReportCheckInvariants(Report) ((VOID)0)
#endif

// Copy core-frame contacts into the final report.
static VOID
AmtSerializeCoreFrameToReport(
    _In_  const PTP_CORE_FRAME* CoreFrame,
    _Out_ PTP_REPORT*           Report
)
{
    UCHAR n = CoreFrame->ContactCount;
    if (n > PTP_MAX_CONTACT_POINTS)
        n = PTP_MAX_CONTACT_POINTS;

    for (UCHAR i = 0; i < n; i++) {
        const PTP_CORE_CONTACT* c = &CoreFrame->Contacts[i];

        Report->Contacts[i].ContactID  = c->ContactID;
        Report->Contacts[i].X          = c->X;
        Report->Contacts[i].Y          = c->Y;
        Report->Contacts[i].TipSwitch  = (c->Phase == CONTACT_PHASE_UP) ? 0 : 1;
        Report->Contacts[i].Confidence = c->Confident ? 1 : 0;
    }

    Report->ContactCount = n;
}

// Queue ClickCount force-touch clicks (each a full down+up pulse) for later
// delivery. ClickCount is 1 for every AMT_POINTER_ACTION_* except
// DOUBLE_CLICK, which needs two independent down+up pulses (Button1 x2) to
// read as a double-click to Windows - reusing this same one-click-at-a-time
// queue instead of inventing a separate multi-click delivery path.
//
// Never touches ForceTouchDeliveryState directly - a click that arrives
// while one is already in flight (or queued) just waits its turn. Only
// clicks that haven't started delivering anything are ever dropped on
// overflow, so a delivered DOWN can never be left without its UP.
// PTPCore_ProcessFrame only ever latches CLICK_ARBITRATION_FORCE_TOUCH via
// the emulation (hold-duration) path when SupportsForceTouch is FALSE -
// real pressure-based hardware always takes the other branch (see
// PTPCore_ProcessFrame). So SupportsForceTouch alone is enough to tell
// which action config a given click was arbitrated under, without
// PTPCore_ProcessFrame having to plumb an extra "which path fired" output
// just for this. Shared by the enqueue site and the delivery site below,
// which may run on different interrupts for the same click and so each
// re-derive the choice rather than carrying it through
// PendingForceTouchClickCount - SupportsForceTouch doesn't change at
// runtime, so both call sites always agree.
static ULONG
AmtPointerResolveForceTapAction(_In_ PDEVICE_CONTEXT pCtx)
{
    return pCtx->SupportsForceTouch
        ? pCtx->PointerConfig.ForceTapAction
        : pCtx->PointerConfig.ForceTouchEmulationAction;
}

static VOID
AmtForceTouchClickEnqueue(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR             ClickCount
)
{
    for (UCHAR i = 0; i < ClickCount; i++) {
        if (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_IDLE &&
            pCtx->PendingForceTouchClickCount == 0) {

            // Nothing in flight and nothing queued - start immediately.
            pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_DOWN_PENDING;
            continue;
        }

        if (pCtx->PendingForceTouchClickCount < PENDING_FORCE_TOUCH_CLICK_CAPACITY) {
            pCtx->PendingForceTouchClickCount++;
        }

        // Else: click storm far beyond anything a human can produce - drop
        // the newest one. Safe: it never started delivering, so nothing is
        // left half-sent.
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t   transferLength = 0;

    // Confirm the device uses a supported packet layout.
    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1:
    case TYPE2:
    case TYPE3:
    case TYPE4:
    case TYPE5:
        transferLength = (size_t)DeviceContext->DeviceInfo->tp_datalen;
        break;

    default:
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    // Validate tp_fsize once at PASSIVE_LEVEL setup time rather than on every
    // USB completion. tp_fsize is fixed for the lifetime of DeviceContext.
    if (transferLength == 0 ||
        DeviceContext->DeviceInfo->tp_fsize == 0) {
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed =
        AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe,
        &contReaderConfig);

exit:
    return status;
}

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx = Context;

    size_t headerSize = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t fingerSize = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t tpDelta    = (unsigned int)pCtx->DeviceInfo->tp_delta;
    size_t raw_n      = 0;

    UCHAR* TouchBuffer = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    PTP_REPORT    Report;

    // USB read completion.

    // fingerSize/headerSize come from the static config entry latched once
    // for this device. The fingerSize==0 case is rejected during setup.
    NT_ASSERT(fingerSize != 0);

    // A successful transfer clears the reader-recovery escalation ladder
    // (AmtPtpEvtUsbInterruptReadersFailed / AmtPtpEvtReaderRestartTimer) -
    // a blip early in a power session should not leave the next unrelated
    // failure escalating straight to a port reset or port cycle.
    //
    // MS-RECOMMENDED SYNCHRONIZATION: this write races the read-modify-
    // write AmtPtpEvtReaderRestartTimer does on the same field under
    // D0ExitLock (Interrupt.c), and the unlocked reads
    // AmtPtpEvtUsbInterruptReadersFailed used to do below - both this
    // routine and ReadersFailed run on the same DPC-level completion path,
    // so an unlocked write here is not just a correctness nicety: without
    // the lock's release/acquire barrier there is no guarantee the timer
    // callback (which can be running on a different CPU) ever observes
    // this reset promptly, or that this write and the timer's own
    // ReaderRecoveryStage++ don't tear against each other. D0ExitLock is a
    // spinlock with no blocking calls inside, so taking it here costs
    // nothing correctness- or performance-wise on the hot path.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    pCtx->ReaderRecoveryStage = READER_RECOVERY_RESET_PIPE;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    // Validate packet shape before dequeuing a HID request and before taking
    // StateLock. This ensures malformed/short USB packets cannot leave an
    // already-dequeued request pending or abandon StateLock.
    //
    // MICRO-OPT: quotient is computed once here (used for the shape check
    // below via a multiply instead of a second division/modulo, and reused
    // as raw_n) instead of separately computing
    // "% fingerSize" for validation and "/ fingerSize" for raw_n further
    // down - fingerSize is a runtime value (from the latched device config,
    // not a compile-time constant), so the compiler cannot fold those into
    // a single hardware divide the way it could for a constant divisor;
    // this guarantees exactly one integer division per interrupt instead
    // of up to two, regardless of compiler CSE behavior across the
    // WdfMemoryGetBuffer call between the old two call sites.
    if (NumBytesTransferred < headerSize ||
        pCtx->DeviceInfo->tp_button >= NumBytesTransferred) {
        return;
    }

    size_t remainingBytes = NumBytesTransferred - headerSize;
    size_t quotient       = remainingBytes / fingerSize;

    if (quotient * fingerSize != remainingBytes) {
        return;
    }

    raw_n = quotient;
    if (raw_n > PTP_MAX_CONTACT_POINTS)
        raw_n = PTP_MAX_CONTACT_POINTS;

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        return;
    }

    // Processing the sensor frame must not depend on whether HIDClass has
    // a read pending at this exact instant. A momentarily empty manual
    // queue is normal and must not stall tracking/velocity/contact lifetime.

    // The continuous reader can re-enter on another CPU. Hold StateLock
    // across the complete state-mutating section.
    WdfSpinLockAcquire(pCtx->StateLock);

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    // KeQueryPerformanceCounter() returns the current QPC tick count;
    // its optional out-parameter contains the frequency.
    Now = KeQueryPerformanceCounter(NULL);

    // Windows PTP expects a free-running 100us scan-time counter.
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;

    PerfDelta =
        (PerfDelta * pCtx->ScanTimeScaleQ16) >> 16;

    if (PerfDelta < 0)
        PerfDelta = 0;

    pCtx->ScanTimeAccumulator += (ULONG)PerfDelta;
    Report.ScanTime = (USHORT)(pCtx->ScanTimeAccumulator & 0xFFFF);
    pCtx->LastReportTime = Now;

    BOOLEAN buttonSnapshot =
        TouchBuffer[pCtx->DeviceInfo->tp_button] != 0;

    // RawFrame construction. AmtInputParseFrame zeroes and fully populates
    // the struct itself (including TimestampQpc).
    RAW_FRAME rawFrame;

    // Every supported configuration reports touch input.
    UCHAR* f_base = TouchBuffer + headerSize + tpDelta;
    AmtInputParseFrame(
        f_base,
        fingerSize,
        raw_n,
        pCtx->DeviceInfo,
        Now.QuadPart,
        &rawFrame);

    // PTPCore orchestration.
    PTP_CORE_FRAME coreFrame;
    BOOLEAN forceTouchClick   = FALSE;
    BOOLEAN buttonClickReport = FALSE;

    PTPCore_ProcessFrame(
        pCtx,
        &rawFrame,
        Now.QuadPart,
        buttonSnapshot,
        &coreFrame,
        &forceTouchClick,
        &buttonClickReport);

    // Serialize to PTP_REPORT.
    AmtSerializeCoreFrameToReport(
        &coreFrame,
        &Report);

    // Hardware Force Touch: do not move the HID contact while arbitration
    // is unresolved. Once HARD_TAP is certain, begin with ButtonDown at
    // the frozen coordinate; subsequent movement is relative to that frame.
    // Never rewrite multi-contact gestures or synthesize contact identities.
    if (pCtx->SupportsForceTouch &&
        pCtx->PointerConfig.ForceTouchEnabled &&
        pCtx->PointerConfig.ForceTouchActionEnabled &&
        Report.ContactCount == 1 &&
        Report.Contacts[0].TipSwitch &&
        (buttonSnapshot || pCtx->ForceMotionHeld)) {
        PTP_CONTACT* contact = &Report.Contacts[0];
        if (buttonSnapshot && (!pCtx->ForceMotionHeld ||
            pCtx->ForceMotionContactId != contact->ContactID)) {
            pCtx->ForceMotionHeld = TRUE;
            pCtx->ForceMotionDragging = FALSE;
            pCtx->ForceMotionContactId = contact->ContactID;
            pCtx->ForceMotionX = contact->X;
            pCtx->ForceMotionY = contact->Y;
        }
        if (pCtx->ForceMotionHeld &&
            !pCtx->ForceMotionDragging && buttonClickReport) {
            pCtx->ForceMotionDragging = TRUE;
            pCtx->ForceMotionResumeX = contact->X;
            pCtx->ForceMotionResumeY = contact->Y;
        }
        if (!pCtx->ForceMotionHeld ||
            pCtx->ForceMotionContactId != contact->ContactID) {
            pCtx->ForceMotionHeld = FALSE;
        } else if (!pCtx->ForceMotionDragging) {
            contact->X = pCtx->ForceMotionX;
            contact->Y = pCtx->ForceMotionY;
        } else {
            LONG x = (LONG)pCtx->ForceMotionX +
                (LONG)contact->X - (LONG)pCtx->ForceMotionResumeX;
            LONG y = (LONG)pCtx->ForceMotionY +
                (LONG)contact->Y - (LONG)pCtx->ForceMotionResumeY;
            contact->X = (USHORT)((x < 0) ? 0 : (x > 65535 ? 65535 : x));
            contact->Y = (USHORT)((y < 0) ? 0 : (y > 65535 ? 65535 : y));
        }
    } else {
        pCtx->ForceMotionHeld = FALSE;
        pCtx->ForceMotionDragging = FALSE;
    }

    // Arbitrated by PTPCore, not the raw button bit.
    if (buttonClickReport) {
        Report.IsButtonClicked = TRUE;
    }

    AmtReportCheckInvariants(&Report);

    // BUG FIX: forceTouchClick only fires the release-frame edge from
    // PTPCore_ProcessFrame - it does not by itself arm the delivery state
    // machine. Without this call ForceTouchDeliveryState never leaves IDLE,
    // so the mouse-delivery block below always computes edgeButtonState ==
    // FALSE and Force Click silently never sends a button pulse. Still
    // under StateLock, matching every other ForceTouchDeliveryState/
    // PendingForceTouchClickCount mutation in this file.
    if (forceTouchClick) {
        ULONG forceTapAction = AmtPointerResolveForceTapAction(pCtx);
        UCHAR clickCount =
            (forceTapAction == AMT_POINTER_ACTION_DOUBLE_CLICK)
                ? 2 : 1;
        AmtForceTouchClickEnqueue(pCtx, clickCount);
    }

    // Capture force-touch delivery state while StateLock is held.
    BOOLEAN needMouseDelivery =
        forceTouchClick ||
        (pCtx->ForceTouchDeliveryState != FORCE_TOUCH_DELIVERY_IDLE);

    // Keep the newest touch report until HIDClass supplies a read. This
    // deliberately coalesces positions rather than blocking the USB reader.
    // StateLock also serializes this slot with QueueReady callbacks.
    pCtx->PendingTouchReport = Report;
    pCtx->PendingTouchReportValid = TRUE;

    WdfSpinLockRelease(pCtx->StateLock);

    // Submit an available read right away; otherwise QueueReady will flush
    // the saved report as soon as HIDClass queues another request.
    AmtPtpTryDeliverPendingTouchReport(pCtx);

    // Optional live monitor snapshot.
    // MICRO-OPT: unlocked peek before taking LiveLock. LiveEnabled is
    // FALSE the overwhelming majority of the time - the live overlay is
    // opt-in and only the GUI's optional live-monitor view turns it on
    // (see the field's comment in Device.h) - so unconditionally acquiring
    // LiveLock here paid for a spinlock acquire/release on every single
    // USB interrupt just to find nothing to do. LiveEnabled is a volatile
    // LONG always written via Interlocked* while LiveLock is held (see
    // AmtPtpSetLiveEnabled / the reader-gone safety net in ConfigIoctl.c),
    // so this unlocked read can't tear; worst case it's stale by one frame
    // right at the enable/disable edge, which only skips or includes one
    // extra live-frame snapshot - never a race on LiveFrame/LiveSequence
    // themselves, since those are only ever touched below, still under
    // the real lock, with LiveEnabled re-checked there.
    if (InterlockedCompareExchange(&pCtx->LiveEnabled, 0, 0) != 0) {
        WdfSpinLockAcquire(pCtx->LiveLock);

        if (pCtx->LiveEnabled) {
            ULONG i;

            PAMT_LIVE_FRAME liveFrame = &pCtx->LiveFrame;

            pCtx->LiveSequence++;

            RtlZeroMemory(
                liveFrame,
                sizeof(*liveFrame));

            liveFrame->StructVersion =
                AMT_LIVE_FRAME_VERSION;

            liveFrame->Sequence =
                pCtx->LiveSequence;

            liveFrame->TimestampQpc =
                Now.QuadPart;

            liveFrame->ContactCount =
                coreFrame.ContactCount;

            liveFrame->RawContactCount =
                rawFrame.ContactCount;

            liveFrame->LargePalmBlanked =
                coreFrame.LargePalmBlanked ? 1 : 0;

            liveFrame->ButtonDown =
                buttonSnapshot ? 1 : 0;

            liveFrame->ForceTouchClick =
                forceTouchClick ? 1 : 0;

            liveFrame->ButtonClickReport =
                buttonClickReport ? 1 : 0;

            for (i = 0;
                 i < coreFrame.ContactCount &&
                 i < AMT_LIVE_MAX_CONTACTS;
                 ++i) {

                ULONG j;
                ULONG bestRawIndex = 0;
                ULONGLONG bestDistance = ~0ULL;
                BOOLEAN haveRawMatch = FALSE;

                // The overlay and the corner calibration need the finger's
                // PHYSICAL (sensor-domain) position. coreFrame X/Y is what
                // Windows is told: scroll scale / cursor speed deliberately
                // move it away from the finger, so it cannot be used to
                // pair the contact with raw sensor data or to draw it.
                // Fall back to the emitted position when the contact is no
                // longer in the pool (lifted this frame - only its UP
                // report is left).
                USHORT sensorX = coreFrame.Contacts[i].X;
                USHORT sensorY = coreFrame.Contacts[i].Y;

                (VOID)AmtContactPoolFindSensorPos(
                    pCtx->ActiveContacts,
                    coreFrame.Contacts[i].ContactID,
                    &sensorX,
                    &sensorY);

                liveFrame->Contacts[i].ContactID =
                    coreFrame.Contacts[i].ContactID;

                liveFrame->Contacts[i].X = sensorX;

                liveFrame->Contacts[i].Y = sensorY;

                liveFrame->Contacts[i].Phase =
                    (ULONG)coreFrame.Contacts[i].Phase;

                liveFrame->Contacts[i].Confident =
                    coreFrame.Contacts[i].Confident ? 1 : 0;

                liveFrame->Contacts[i].PalmSuspect =
                    coreFrame.Contacts[i].PalmSuspect ? 1 : 0;

                for (j = 0;
                     j < rawFrame.ContactCount;
                     ++j) {

                    LONGLONG dx =
                        (LONGLONG)sensorX -
                        (LONGLONG)rawFrame.Contacts[j].X;

                    LONGLONG dy =
                        (LONGLONG)sensorY -
                        (LONGLONG)rawFrame.Contacts[j].Y;

                    ULONGLONG distance =
                        (ULONGLONG)(dx * dx + dy * dy);

                    if (!haveRawMatch ||
                        distance < bestDistance) {

                        haveRawMatch = TRUE;
                        bestDistance = distance;
                        bestRawIndex = j;
                    }
                }

                if (haveRawMatch) {
                    liveFrame->Contacts[i].RawX =
                        rawFrame.Contacts[bestRawIndex].RawX;

                    liveFrame->Contacts[i].RawY =
                        rawFrame.Contacts[bestRawIndex].RawY;

                    liveFrame->Contacts[i].Major =
                        rawFrame.Contacts[bestRawIndex].Major;

                    liveFrame->Contacts[i].Minor =
                        rawFrame.Contacts[bestRawIndex].Minor;

                    liveFrame->Contacts[i].Pressure =
                        rawFrame.Contacts[bestRawIndex].Pressure;

                    liveFrame->Contacts[i].Orientation =
                        rawFrame.Contacts[bestRawIndex].Orientation;
                } else {
                    liveFrame->Contacts[i].RawX =
                        (SHORT)((LONG)sensorX +
                                pCtx->DeviceInfo->x.min);

                    liveFrame->Contacts[i].RawY =
                        (SHORT)(pCtx->DeviceInfo->y.max -
                                (LONG)sensorY);

                    liveFrame->Contacts[i].Major = 0;
                    liveFrame->Contacts[i].Minor = 0;
                }
            }
        }

        WdfSpinLockRelease(pCtx->LiveLock);
    }

    // Mouse delivery is a separate critical section around the force-touch
    // delivery state machine. Opportunistically claim a second pending
    // IOCTL_HID_READ_REPORT request off the SAME manual InputQueue used by
    // the digitizer/touch client above - mouhid.sys keeps its own read
    // continuously queued there too. See AmtPtpTryDeliverForceTouchClick
    // (below) for the actual attempt; this call is the touch-interrupt side
    // of its two trigger points - the other is AmtPtpEvtInputQueueReady,
    // which covers the case where nothing is left touching the pad to
    // generate another interrupt here.
    if (needMouseDelivery) {
        AmtPtpTryDeliverForceTouchClick(pCtx);
    }
}

// Coalesce sensor positions while no read is available, then deliver the
// newest report. A request is never held hostage waiting for another USB
// frame. All slot operations and dequeues are serialized by StateLock.
static VOID
AmtPtpTryDeliverPendingTouchReport(_In_ PDEVICE_CONTEXT pCtx)
{
    WDFREQUEST request = NULL;
    WDFMEMORY memory;
    NTSTATUS status;

    WdfSpinLockAcquire(pCtx->StateLock);
    if (!pCtx->PendingTouchReportValid) {
        WdfSpinLockRelease(pCtx->StateLock);
        return;
    }

    status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &request);
    if (!NT_SUCCESS(status)) {
        WdfSpinLockRelease(pCtx->StateLock);
        return;
    }

    status = WdfRequestRetrieveOutputMemory(request, &memory);
    if (NT_SUCCESS(status)) {
        status = WdfMemoryCopyFromBuffer(
            memory, 0, &pCtx->PendingTouchReport, sizeof(PTP_REPORT));
        if (NT_SUCCESS(status)) {
            WdfRequestSetInformation(request, sizeof(PTP_REPORT));
            pCtx->PendingTouchReportValid = FALSE;
        }
    }
    WdfSpinLockRelease(pCtx->StateLock);
    WdfRequestComplete(request, status);
}

// AmtPtpTryDeliverForceTouchClick
//
// One attempt at advancing the force-touch mouse-click delivery state
// machine (ForceTouchDeliveryState) by claiming a pending
// IOCTL_HID_READ_REPORT request off InputQueue and writing the next
// DOWN/UP pulse into it. No-ops immediately if nothing is currently in
// flight (FORCE_TOUCH_DELIVERY_IDLE) or if InputQueue has no request
// ready right now - the caller is expected to be one of this function's
// two trigger points, each covering a different way for "no request
// ready right now" to later become "one is ready":
//
//   - AmtPtpEvtUsbInterruptPipeReadComplete (above): a new touch report
//     arrived, so worth trying again - covers the common case where the
//     finger is still on the pad generating a steady stream of reports.
//   - AmtPtpEvtInputQueueReady (Queue.c registration, this file): a new
//     request just landed on InputQueue while it was empty. This is the
//     one that actually closes the gap: if the finger already lifted
//     (no more touch interrupts to retry on) at the exact moment
//     mouhid.sys's read wasn't queued yet, this callback is what
//     delivers the pending click instead of leaving it stuck until the
//     next unrelated touch.
//
// Both trigger points may fire for the same pending click; whichever
// wins the race to WdfIoQueueRetrieveNextRequest first delivers it, and
// the other's attempt then finds ForceTouchDeliveryState back at IDLE
// (or the queue empty again) and no-ops. StateLock serializes the two.
static VOID
AmtPtpTryDeliverForceTouchClick(
    _In_ PDEVICE_CONTEXT pCtx)
{
    NTSTATUS   Status;
    WDFREQUEST mouseRequest = NULL;

    WdfSpinLockAcquire(pCtx->StateLock);

    if (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_IDLE) {
        WdfSpinLockRelease(pCtx->StateLock);
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(
        pCtx->InputQueue,
        &mouseRequest);

    if (NT_SUCCESS(Status)) {
        WDFMEMORY mouseRequestMemory;

        Status = WdfRequestRetrieveOutputMemory(
            mouseRequest,
            &mouseRequestMemory);

        if (NT_SUCCESS(Status)) {
            PTP_FORCETOUCH_MOUSE_REPORT mouseReport;

            BOOLEAN edgeButtonState =
                (pCtx->ForceTouchDeliveryState ==
                 FORCE_TOUCH_DELIVERY_DOWN_PENDING);

            RtlZeroMemory(
                &mouseReport,
                sizeof(mouseReport));

            mouseReport.ReportID =
                REPORTID_STANDARDMOUSE;

            switch (AmtPointerResolveForceTapAction(pCtx)) {
            case AMT_POINTER_ACTION_MIDDLE_CLICK:
                mouseReport.Button3 =
                    edgeButtonState ? 1 : 0;
                break;

            case AMT_POINTER_ACTION_DOUBLE_CLICK:
                mouseReport.Button1 =
                    edgeButtonState ? 1 : 0;
                break;

            case AMT_POINTER_ACTION_CONTEXT_MENU:
            default:
                mouseReport.Button2 =
                    edgeButtonState ? 1 : 0;
                break;
            }

            Status = WdfMemoryCopyFromBuffer(
                mouseRequestMemory,
                0,
                (PVOID)&mouseReport,
                sizeof(mouseReport));

            if (NT_SUCCESS(Status)) {
                WdfRequestSetInformation(
                    mouseRequest,
                    sizeof(mouseReport));

                if (pCtx->ForceTouchDeliveryState ==
                    FORCE_TOUCH_DELIVERY_DOWN_PENDING) {

                    pCtx->ForceTouchDeliveryState =
                        FORCE_TOUCH_DELIVERY_UP_PENDING;

                } else {
                    pCtx->ForceTouchDeliveryState =
                        FORCE_TOUCH_DELIVERY_IDLE;

                    if (pCtx->PendingForceTouchClickCount > 0) {
                        pCtx->PendingForceTouchClickCount--;

                        pCtx->ForceTouchDeliveryState =
                            FORCE_TOUCH_DELIVERY_DOWN_PENDING;
                    }
                }
            }
        }
    }

    WdfSpinLockRelease(pCtx->StateLock);

    if (mouseRequest != NULL) {
        WdfRequestComplete(
            mouseRequest,
            Status);
    }
}

// AmtPtpEvtInputQueueReady
//
// Called when InputQueue transitions from empty to non-empty. A queued
// HID read may be the only opportunity to deliver a pending Force Touch
// DOWN/UP after the final physical touch interrupt. Always enter the
// StateLock-protected delivery helper: an unsynchronized IDLE pre-check
// here could miss that sole notification and strand the pending report.
VOID
AmtPtpEvtInputQueueReady(
    _In_ WDFQUEUE    Queue,
    _In_ WDFCONTEXT  Context)
{
    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Queue);

    AmtPtpTryDeliverPendingTouchReport(pCtx);
    AmtPtpTryDeliverForceTouchClick(pCtx);
}

// AmtPtpCyclePort
//
// Last rung of the reader-recovery escalation ladder - see
// READER_RECOVERY_STAGE in Device.h. Power-cycles the device's USB port,
// the moral equivalent of an unplug/replug: the device disappears and
// reappears through normal PnP. A WDFUSB wrapper for this does exist -
// WdfUsbTargetDeviceCyclePortSynchronously(WDFUSBDEVICE) - but per its
// documented signature it takes no WDFREQUEST/WDF_REQUEST_SEND_OPTIONS
// parameters, so it cannot be given a time-out. This is the last rung of
// the ladder and, unlike a stuck endpoint or a bad port, has no fallback
// if it never completes, so it deliberately goes down as the raw internal
// IOCTL against the WDFUSBDEVICE's own I/O target instead, purely to keep
// the bounded WDF_REQUEST_SEND_OPTION_TIMEOUT below.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCyclePort(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDFIOTARGET               ioTarget;
    WDF_REQUEST_SEND_OPTIONS  sendOptions;

    PAGED_CODE();

    ioTarget = WdfUsbTargetDeviceGetIoTarget(DeviceContext->UsbDevice);

    // Bounded like the Wellspring control transfers in Device.c - a cycle
    // request that never completes must not hang the passive-level
    // recovery worker (AmtPtpEvtReaderRestartTimer) forever.
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &sendOptions, WDF_REL_TIMEOUT_IN_SEC(WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC));

    return WdfIoTargetSendInternalIoctlSynchronously(
        ioTarget,
        NULL,                        // Request - let WDF allocate one
        IOCTL_INTERNAL_USB_CYCLE_PORT,
        NULL,                        // InputBuffer
        NULL,                        // OutputBuffer
        &sendOptions,
        NULL);                       // BytesReturned
}

// AmtPtpEvtReaderRestartTimer
//
// Performs one rung of the READER_RECOVERY_STAGE escalation ladder, then
// tries to resume the continuous reader. Runs at PASSIVE_LEVEL (the timer
// object was created with ExecutionLevel = WdfExecutionLevelPassive and
// AutomaticSerialization = FALSE - see Device.c - specifically for this:
// WdfUsbTargetPipeResetSynchronously, WdfUsbTargetDeviceResetPortSynchronously,
// AmtPtpCyclePort, and WdfWaitLockAcquire below are all PASSIVE_LEVEL-only
// blocking calls).
//
// SYNCHRONIZATION SHAPE (see the two-level model comment in Device.h):
// a snapshot taken under D0ExitLock alone is not, by itself, a lifetime
// guarantee - D0Exit/ReleaseHardware could still start and finish an entire
// teardown between this function's snapshot and its actual use of that
// snapshot a few calls later. So every use of InterruptPipe/UsbDevice for
// an actual (possibly blocking) recovery step happens only after this
// function itself holds RecoveryLock, with the D0ExitInProgress/
// RecoveryGeneration check *repeated* once RecoveryLock is held - closing
// the window a single up-front check would leave open.
VOID
AmtPtpEvtReaderRestartTimer(
    _In_ WDFTIMER Timer)
{
    WDFDEVICE       device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT pCtx   = DeviceGetContext(device);
    WDFUSBPIPE      localInterruptPipe;
    ULONG           snapshotGeneration;
    READER_RECOVERY_STAGE stage;

    _Analysis_assume_(pCtx != NULL);

    // The timer is configured for PASSIVE_LEVEL execution, but this callback
    // briefly acquires D0ExitLock, so it intentionally remains resident.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    if (pCtx->D0ExitInProgress) {
        WdfSpinLockRelease(pCtx->D0ExitLock);
        AmtTrace(pCtx, "ReaderRestartTimer: D0Exit in progress, backing off");
        return;
    }

    snapshotGeneration = pCtx->RecoveryGeneration;
    stage = pCtx->ReaderRecoveryStage;
    localInterruptPipe = pCtx->InterruptPipe;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    AmtTrace(pCtx, "ReaderRestartTimer: ENTER, stage=%d", (int)stage);

    if (!AmtPtpRecoveryLockAcquireBounded(pCtx, "ReaderRestartTimer")) {
        AmtTrace(pCtx,
            "ReaderRestartTimer: RecoveryLock acquire timed out, deferring this recovery fire");
        WdfTimerStart(
            pCtx->ReaderRestartTimer,
            WDF_REL_TIMEOUT_IN_MS(READER_RECOVERY_STEP_DELAY_MS));
        return;
    }

    // Re-validate lifecycle after taking RecoveryLock. D0Exit/ReleaseHardware
    // can race with the timer between the Phase-1 snapshot and lock acquire.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    if (pCtx->D0ExitInProgress ||
        pCtx->RecoveryGeneration != snapshotGeneration) {
        WdfSpinLockRelease(pCtx->D0ExitLock);
        AmtTrace(pCtx, "ReaderRestartTimer: lifecycle changed since snapshot, aborting");
        WdfWaitLockRelease(pCtx->RecoveryLock);
        return;
    }
    stage = pCtx->ReaderRecoveryStage;
    localInterruptPipe = pCtx->InterruptPipe;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    // T2 reader failures no longer route through this timer at all:
    // AmtPtpEvtUsbInterruptReadersFailed calls WdfDeviceSetFailed directly
    // as soon as it observes a T2 post-resume failure, instead of arming
    // this timer as a 1ms execution bridge. This timer is now reachable
    // only by non-T2 devices (the bounded reset ladder below) and by the
    // RecoveryLock-timeout retry above, so no T2 branch remains here.
    //
    // Non-T2 devices retain the existing bounded reset ladder.
    if (stage >= READER_RECOVERY_EXHAUSTED) {
        AmtTrace(pCtx,
            "ReaderRestartTimer: ladder EXHAUSTED, giving up until next D0Entry");
        WdfWaitLockRelease(pCtx->RecoveryLock);
        return;
    }

    if (localInterruptPipe == NULL || pCtx->UsbDevice == NULL) {
        AmtTrace(pCtx,
            "ReaderRestartTimer: USB handles unavailable, requesting PnP re-enumeration");
        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);
        WdfWaitLockRelease(pCtx->RecoveryLock);
        WdfDeviceSetFailed(device, WdfDeviceFailedAttemptRestart);
        return;
    }

    if (!NT_SUCCESS(WdfUsbTargetDeviceIsConnectedSynchronous(pCtx->UsbDevice))) {
        AmtTrace(pCtx,
            "ReaderRestartTimer: device not connected, skipping reset ladder, "
            "requesting PnP restart via WdfDeviceSetFailed");
        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);
        WdfWaitLockRelease(pCtx->RecoveryLock);
        WdfDeviceSetFailed(device, WdfDeviceFailedAttemptRestart);
        return;
    }

    // All reset rungs require the interrupt-pipe I/O target to be stopped.
    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(localInterruptPipe),
        WdfIoTargetCancelSentIo);

    if (stage == READER_RECOVERY_CYCLE_PORT) {
        NTSTATUS rungStatus = AmtPtpCyclePort(pCtx);
        AmtTrace(pCtx,
            "ReaderRestartTimer: CYCLE_PORT -> status=0x%08X, handing lifecycle to PnP",
            rungStatus);

        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);
        WdfWaitLockRelease(pCtx->RecoveryLock);
        return;
    }

    switch (stage) {
    case READER_RECOVERY_RESET_PIPE: {
        NTSTATUS rungStatus = WdfUsbTargetPipeResetSynchronously(
            localInterruptPipe,
            WDF_NO_HANDLE,
            NULL);
        AmtTrace(pCtx,
            "ReaderRestartTimer: RESET_PIPE -> status=0x%08X", rungStatus);
        break;
    }

    case READER_RECOVERY_RESET_PORT:
    default: {
        NTSTATUS rungStatus = WdfUsbTargetDeviceResetPortSynchronously(
            pCtx->UsbDevice);
        AmtTrace(pCtx,
            "ReaderRestartTimer: RESET_PORT -> status=0x%08X", rungStatus);
        break;
    }
    }

    {
        READER_RECOVERY_STAGE nextStage = stage;

        WdfSpinLockAcquire(pCtx->D0ExitLock);
        if (pCtx->RecoveryGeneration == snapshotGeneration) {
            pCtx->ReaderRecoveryStage++;
            nextStage = pCtx->ReaderRecoveryStage;
        }
        WdfSpinLockRelease(pCtx->D0ExitLock);

        // Restart even when the reset operation itself failed. A persistent
        // reader failure will re-enter ReadersFailed and advance the ladder.
        NTSTATUS restartStatus = WdfIoTargetStart(
            WdfUsbTargetPipeGetIoTarget(localInterruptPipe));
        AmtTrace(pCtx,
            "ReaderRestartTimer: EXIT, WdfIoTargetStart -> status=0x%08X, next stage=%d",
            restartStatus,
            (int)nextStage);
    }

    WdfWaitLockRelease(pCtx->RecoveryLock);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    WDFIOTARGET      ioTarget = WdfUsbTargetPipeGetIoTarget(Pipe);
    WDFDEVICE        device   = WdfIoTargetGetDevice(ioTarget);
    PDEVICE_CONTEXT  pCtx     = DeviceGetContext(device);
    BOOLEAN                d0ExitInProgress;
    BOOLEAN                t2ResumeActive;
    READER_RECOVERY_STAGE  stage;

    // See the identical PREfast note in AmtPtpDeviceUsbKmEvtDevicePrepareHardware (Device.c).
    _Analysis_assume_(pCtx != NULL);

    // MS-RECOMMENDED SYNCHRONIZATION: this is the second, previously-
    // unguarded path into ReaderRestartTimer - see the D0ExitLock comment
    // in Device.h. WdfIoTargetStop(..., WdfIoTargetCancelSentIo) in
    // AmtPtpEvtDeviceD0Exit cancels any reads still in flight on this pipe,
    // and per the documented continuous-reader contract that cancellation
    // itself is what drives this very callback (EvtUsbTargetPipeReadersFailed
    // fires "after all read requests have been completed", cancellation
    // included) - so this can run synchronously *inside* D0Exit's
    // WdfIoTargetStop call, i.e. after D0ExitInProgress is already set but
    // before D0Exit has finished. Calling WdfTimerStart here unconditionally
    // would re-arm the exact timer D0Exit just tried to stop.
    //
    // D0ExitInProgress and ReaderRecoveryStage are snapshotted together
    // under one D0ExitLock critical section, not as two separate
    // lock/unlock pairs - a previous revision checked D0ExitInProgress,
    // released the lock, and only then read ReaderRecoveryStage
    // unprotected. That left a window where D0Exit could set
    // D0ExitInProgress and the timer callback could concurrently mutate
    // ReaderRecoveryStage between the two reads, and - on top of the
    // torn-snapshot risk - an unlocked read has no ordering guarantee that
    // this callback (which may run on a different CPU than the timer
    // callback that last wrote the field) observes the latest value at
    // all. Take the same D0ExitLock D0Exit/ReaderRestartTimer use - this
    // callback can run at up to DISPATCH_LEVEL (same completion path as
    // EvtUsbTargetPipeReadComplete), which is exactly why this is a
    // WDFSPINLOCK and not a WDFWAITLOCK.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    d0ExitInProgress = pCtx->D0ExitInProgress;
    t2ResumeActive   = pCtx->T2ResumeActive;
    stage            = pCtx->ReaderRecoveryStage;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    // DIAG: this is the very first callback to see a broken interrupt
    // read - Status/UsbdStatus here are the actual root cause of any
    // downstream Code 10, so they're worth capturing even though the
    // recovery logic below doesn't otherwise need their exact values.
    // Uses the just-snapshotted local, not a fresh unlocked field read.
    AmtTrace(pCtx, "ReadersFailed: Status=0x%08X, UsbdStatus=0x%08X, stage=%d",
        Status, UsbdStatus, (int)stage);

    if (d0ExitInProgress) {
        AmtTrace(pCtx, "ReadersFailed: D0Exit in progress, not arming restart timer");
        return FALSE;
    }

    if (AmtPtpIsT2Device(pCtx)) {
        // T2 does not recover the Wellspring endpoint in-place across the
        // ordinary D3 -> D0 transition.  The known-good path is a complete
        // PnP re-enumeration: ReleaseHardware -> PrepareHardware -> D0Entry.
        //
        // Call WdfDeviceSetFailed directly from this callback instead of
        // bouncing through the PASSIVE_LEVEL ReaderRestartTimer as a 1ms
        // execution bridge. WdfDeviceSetFailed is documented IRQL <=
        // DISPATCH_LEVEL, and this callback itself runs at up to
        // DISPATCH_LEVEL, so no bridge is required for IRQL correctness.
        // WdfDeviceSetFailed only latches failed state and schedules PnP
        // teardown asynchronously - it does not block and does not touch
        // RecoveryLock, so it is also safe to call from the rare case where
        // this callback fires synchronously out of WdfIoTargetStart while
        // AmtPtpEvtDeviceD0Entry is still on the stack holding RecoveryLock
        // (see the D0Entry comment on synchronous T2 reader failure).
        //
        // Removing the timer hop removes the extra scheduling + RecoveryLock
        // round-trip that used to sit between "failure observed" and
        // "WdfDeviceSetFailed called" - that gap was time during which the
        // stale T2 HID stack could still accept an IOCTL (rejected with
        // STATUS_DEVICE_NOT_READY via the T2RestartPending gate in Queue.c)
        // and surface as MTConfig Event ID 1 before PnP actually tore the
        // old stack down.
        // Idempotency guard: if a prior ReadersFailed callback in this same
        // power session already latched T2RestartPending, WdfDeviceSetFailed
        // has already been called once for this failure episode and PnP
        // teardown may already be in flight. Multiple queued/cancelled reads
        // completing close together can drive this callback more than once
        // before that teardown actually removes the old stack, so do not
        // call WdfDeviceSetFailed a second time - that used to be implicitly
        // guarded by ReaderRestartTimer's RecoveryGeneration re-check, which
        // this direct-call path has no equivalent of otherwise.
        BOOLEAN alreadyPending;
        WdfSpinLockAcquire(pCtx->D0ExitLock);
        alreadyPending = pCtx->T2RestartPending;
        pCtx->T2RestartPending = TRUE;
        pCtx->T2ResumeActive = FALSE;
        WdfSpinLockRelease(pCtx->D0ExitLock);

        if (alreadyPending) {
            AmtTrace(pCtx,
                "ReadersFailed: T2 failure (Status=0x%08X) already pending, "
                "WdfDeviceSetFailed already called, skipping duplicate call",
                Status);
            return FALSE;
        }

        AmtTrace(pCtx,
            "ReadersFailed: T2 post-resume reader failure (Status=0x%08X), "
            "calling WdfDeviceSetFailed directly (resumeActive=%u)",
            Status, t2ResumeActive ? 1u : 0u);

        // WdfDeviceSetFailed can set the old device stack's teardown in
        // motion. Do not touch device/pCtx after this call.
        WdfDeviceSetFailed(device, WdfDeviceFailedAttemptRestart);
        return FALSE;
    }

    // Escalating recovery (reset-pipe -> reset-port -> cycle-port) instead
    // of an immediate unconditional restart. Returning TRUE unconditionally
    // here previously let a persistently failing pipe (most commonly:
    // right after a system resume, before the endpoint has settled) spin
    // as fast as USBD could complete failed URBs - real CPU and
    // USB-controller cost, observable as elevated temperature after wake -
    // and never actually attempted the recovery a stuck endpoint or port
    // typically needs.
    if (stage >= READER_RECOVERY_EXHAUSTED) {
        // Give up until the next D0Entry resets the ladder. The device
        // stays silent rather than burning power in an unbounded loop;
        // AmtPtpEvtDeviceD0Entry (a genuine replug, or the next sleep/wake
        // cycle) gets a clean slate to try again.
        return FALSE;
    }

    WdfTimerStart(
        pCtx->ReaderRestartTimer,
        WDF_REL_TIMEOUT_IN_MS(READER_RECOVERY_STEP_DELAY_MS));

    // We own the recovery/restart via the timer above, so tell the
    // framework not to immediately resubmit the reader itself..
    return FALSE;
}