using System;
using System.Diagnostics;
using System.Threading;
using AmtPtpConfigGui.Native;

namespace AmtPtpConfigGui
{
    internal enum HapticsState
    {
        // Nothing has been asked of the monitor yet (StatusWanted is off).
        Unknown,
        // The driver's config has the second click (or Force Touch) off.
        Disabled,
        // The Wellspring touchpad driver is not connected.
        DriverUnavailable,
        // No actuator driver interface is present.
        ActuatorUnavailable,
        // The actuator driver speaks another interface version.
        ActuatorIncompatible,
        // The actuator driver is there, but the touchpad driver has no open
        // link to it (yet): a hard press would not click.
        LinkDown,
        FeedUnavailable,
        // On and playable; the live pressure meter is not being fed (it is
        // only streamed while the Live switch is on).
        Idle,
        // On and playable, live pressure meter being fed.
        Running,
    }

    /// <summary>
    /// What the Haptics group shows. Clicks* are the touchpad driver's own
    /// counters since its device started (zero when it cannot report them).
    /// </summary>
    internal readonly record struct HapticsStatus(
        HapticsState State,
        int Pressure,
        uint ClicksDelivered,
        uint ClicksFailed);

    /// <summary>
    /// Companion of the second haptic click inside the control panel. The
    /// click itself is decided AND played by the touchpad driver (it works
    /// without this app), and the settings are ordinary driver configuration
    /// (PointerConfig). This class only observes and assists:
    ///   (1) status - asks the touchpad driver whether the click is switched
    ///       on and whether its link to the actuator driver is ready
    ///       (falling back to probing the actuator interface for drivers
    ///       that cannot say), only while the group is visible
    ///       (StatusWanted);
    ///   (2) "Test click" - one click sent straight to the actuator driver;
    ///   (3) the live pressure meter - reads the driver's live frames
    ///       (shared with the Live view through DeviceIo.SetLiveConsumer),
    ///       only while the meter is visible (MeterWanted).
    /// It asks the drivers for nothing while neither is wanted, and it never
    /// keeps the actuator open (see HapticsActuator).
    /// </summary>
    internal sealed class HapticsEngine : IDisposable
    {
        private const int PollMs = 8;
        private const int StatusMs = 250;
        private const int RetryMs = 500;
        private const int StaleMs = 250;

        // How long a status answer is reused before the drivers are asked
        // again: the meter loop runs every PollMs, the status is not worth
        // two IOCTLs on each of those turns.
        private const int StatusRefreshMs = 500;

        // Probing the actuator means enumerating its device interface and
        // opening it; on a machine without the actuator driver that is asked
        // for on every status round, so its answer is reused for longer.
        private const int ProbeRefreshMs = 2000;

        private readonly DeviceIo _device;
        private readonly CancellationTokenSource _cts = new CancellationTokenSource();
        private readonly AutoResetEvent _wake = new AutoResetEvent(false);
        private readonly Thread _thread;

        private volatile bool _statusWanted;
        private volatile bool _meterWanted;

        private int _state = (int)HapticsState.Unknown;
        private int _pressure;
        private int _clicksDelivered;
        private int _clicksFailed;

        // Worker thread only.
        private long _probeAtMs = long.MinValue;
        private HapticsActuator.Result _probeResult = HapticsActuator.Result.NotInstalled;

        public HapticsEngine(DeviceIo device)
        {
            _device = device;
            _thread = new Thread(Run)
            {
                IsBackground = true,
                Name = "HapticsEngine",
            };
            _thread.Start();
        }

        public HapticsStatus Status => new HapticsStatus(
            (HapticsState)Volatile.Read(ref _state),
            Volatile.Read(ref _pressure),
            unchecked((uint)Volatile.Read(ref _clicksDelivered)),
            unchecked((uint)Volatile.Read(ref _clicksFailed)));

        /// <summary>
        /// Set by the UI while the Haptics group is visible. Only then are the
        /// drivers asked for their state; otherwise the worker sleeps.
        /// </summary>
        public bool StatusWanted
        {
            get => _statusWanted;
            set
            {
                if (_statusWanted == value)
                    return;
                _statusWanted = value;
                if (value)
                    _wake.Set();
            }
        }

        /// <summary>
        /// Set by the UI while the live pressure meter is visible. Only then
        /// is the driver asked for live frames. Has no effect without
        /// StatusWanted.
        /// </summary>
        public bool MeterWanted
        {
            get => _meterWanted;
            set
            {
                if (_meterWanted == value)
                    return;
                _meterWanted = value;
                if (value)
                    _wake.Set();
            }
        }

        /// <summary>
        /// Fires one click now, saved settings or not, so a waveform can be
        /// judged before pressing Save. Independent of the driver's config
        /// and of the status loop. Blocks for the transfer: call it off the
        /// UI thread.
        /// </summary>
        public static HapticsActuator.Result TestClick(int waveform) =>
            HapticsActuator.Click(waveform);

        /// <summary>
        /// Maps what the drivers reported to the state shown in the UI. Pure,
        /// so the decision table can be tested without any driver.
        /// </summary>
        /// <param name="connected">The touchpad driver's control device is open.</param>
        /// <param name="pointer">Its current pointer config, or null if unreadable.</param>
        /// <param name="link">Its haptic link info, or null if it cannot report one (older driver).</param>
        /// <param name="probe">Lazily probes the actuator interface; only called when needed.</param>
        internal static HapticsState Evaluate(
            bool connected,
            PointerConfig? pointer,
            HapticLinkInfo? link,
            Func<HapticsActuator.Result> probe)
        {
            if (!connected || pointer == null)
                return HapticsState.DriverUnavailable;

            PointerConfig cfg = pointer.Value;
            if (cfg.ForceTouchEnabled == 0 || cfg.ForceTouchHapticEnabled == 0)
                return HapticsState.Disabled;

            if (link != null)
            {
                switch (link.Value.State)
                {
                    case HapticLinkInfo.StateReady:
                        return HapticsState.Idle;
                    case HapticLinkInfo.StateIncompatible:
                        return HapticsState.ActuatorIncompatible;
                    case HapticLinkInfo.StateClosed:
                        return HapticsState.LinkDown;
                }

                // Absent: tell "not installed" from "installed, but the
                // touchpad driver has not linked to it" with our own probe.
                return probe() switch
                {
                    HapticsActuator.Result.Ok => HapticsState.LinkDown,
                    HapticsActuator.Result.Incompatible => HapticsState.ActuatorIncompatible,
                    _ => HapticsState.ActuatorUnavailable,
                };
            }

            // A touchpad driver that cannot report its link: the best
            // available answer is whether the actuator driver is there.
            return probe() switch
            {
                HapticsActuator.Result.Ok => HapticsState.Idle,
                HapticsActuator.Result.Incompatible => HapticsState.ActuatorIncompatible,
                _ => HapticsState.ActuatorUnavailable,
            };
        }

        private void Run()
        {
            var clock = Stopwatch.StartNew();
            CancellationToken token = _cts.Token;
            WaitHandle[] waits = { token.WaitHandle, _wake };
            uint sequence = 0;
            long lastFrameMs = 0;
            long statusAtMs = long.MinValue;
            HapticsState baseState = HapticsState.Unknown;
            bool liveHeld = false;

            // Also right when the device is no longer connected: the call
            // then only clears this consumer's bit, so a later reconnect does
            // not inherit a wish nobody holds any more.
            void ReleaseLive()
            {
                if (liveHeld)
                {
                    _device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, false);
                    liveHeld = false;
                }
            }

            try
            {
                while (!token.IsCancellationRequested)
                {
                    if (!_statusWanted)
                    {
                        // Nobody is looking: give the live stream back and
                        // sleep until the group is shown again.
                        ReleaseLive();
                        Publish(HapticsState.Unknown, 0);
                        statusAtMs = long.MinValue;
                        WaitHandle.WaitAny(waits);
                        continue;
                    }

                    long now = clock.ElapsedMilliseconds;

                    if (statusAtMs == long.MinValue || now - statusAtMs >= StatusRefreshMs)
                    {
                        baseState = QueryState(now);
                        statusAtMs = now;
                    }

                    bool playable = baseState == HapticsState.Idle;

                    // The meter only makes sense while a hard press can click;
                    // otherwise do not make the driver assemble live frames.
                    if (!playable || !_meterWanted)
                    {
                        ReleaseLive();
                        Publish(baseState, 0);
                        WaitHandle.WaitAny(waits, playable ? StatusMs : RetryMs);
                        continue;
                    }

                    // Cheap when nothing changed; re-sent after a reconnect or
                    // a failed read.
                    if (!liveHeld)
                    {
                        if (!_device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, true))
                        {
                            Publish(HapticsState.FeedUnavailable, 0);
                            WaitHandle.WaitAny(waits, RetryMs);
                            continue;
                        }

                        liveHeld = true;
                        sequence = 0;
                        lastFrameMs = now;
                    }

                    if (!_device.TryGetLiveFrame(out LiveFrame frame))
                    {
                        // Drop our share properly (not just the local flag):
                        // a consumer bit left set would keep the driver
                        // streaming for nobody once the Live view lets go.
                        ReleaseLive();
                        Publish(HapticsState.FeedUnavailable, 0);
                        statusAtMs = long.MinValue;     // find out why right away
                        WaitHandle.WaitAny(waits, RetryMs);
                        continue;
                    }

                    if (frame.Sequence == sequence)
                    {
                        // No new frame. After a quiet spell forget the last
                        // sample so a stale pressure does not stay on screen.
                        if (now - lastFrameMs > StaleMs)
                            Publish(HapticsState.Running, 0);

                        token.WaitHandle.WaitOne(PollMs);
                        continue;
                    }

                    sequence = frame.Sequence;
                    lastFrameMs = now;

                    bool valid = frame.Contacts != null
                        && frame.Contacts.Length > 0
                        && frame.ContactCount == 1
                        && frame.RawContactCount == 1
                        && frame.LargePalmBlanked == 0
                        && frame.Contacts[0].Confident != 0
                        && frame.Contacts[0].PalmSuspect == 0;
                    int pressure = valid ? frame.Contacts![0].Pressure : 0;

                    Publish(HapticsState.Running, pressure);

                    token.WaitHandle.WaitOne(PollMs);
                }
            }
            finally
            {
                ReleaseLive();
            }
        }

        // One round of questions to the drivers: two small IOCTLs to the
        // touchpad driver and, only when its answer leaves it open, a (cached)
        // probe of the actuator driver.
        private HapticsState QueryState(long now)
        {
            bool connected = _device.IsConnected;

            PointerConfig? pointer = null;
            HapticLinkInfo? link = null;

            if (connected)
            {
                if (_device.TryGetPointerConfig(out PointerConfig cfg))
                    pointer = cfg;

                if (_device.TryGetHapticLinkInfo(out HapticLinkInfo info))
                {
                    link = info;
                    Volatile.Write(ref _clicksDelivered, unchecked((int)info.ClicksDelivered));
                    Volatile.Write(ref _clicksFailed, unchecked((int)info.ClicksFailed));
                }
                else
                {
                    Volatile.Write(ref _clicksDelivered, 0);
                    Volatile.Write(ref _clicksFailed, 0);
                }
            }

            return Evaluate(connected, pointer, link, () => ProbeCached(now));
        }

        private HapticsActuator.Result ProbeCached(long now)
        {
            if (_probeAtMs == long.MinValue || now - _probeAtMs >= ProbeRefreshMs)
            {
                _probeResult = HapticsActuator.Probe(out _);
                _probeAtMs = now;
            }

            return _probeResult;
        }

        private void Publish(HapticsState state, int pressure)
        {
            Volatile.Write(ref _state, (int)state);
            Volatile.Write(ref _pressure, pressure);
        }

        public void Dispose()
        {
            _cts.Cancel();

            // Disposing the wait handles under a worker that is still waiting
            // on them would throw on that thread; if it did not stop in time,
            // leave them to the finalizer instead.
            if (_thread.Join(1000))
            {
                _cts.Dispose();
                _wake.Dispose();
            }
        }
    }
}
