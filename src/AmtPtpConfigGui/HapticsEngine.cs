using System;
using System.Diagnostics;
using System.Threading;
using AmtPtpConfigGui.Native;
using ActuatorHandle = Microsoft.Win32.SafeHandles.SafeFileHandle;

namespace AmtPtpConfigGui
{
    internal enum HapticsState
    {
        Disabled,
        DriverUnavailable,
        ActuatorUnavailable,
        FeedUnavailable,
        Running,
    }

    internal readonly record struct HapticsStatus(HapticsState State, int Pressure, int Clicks);

    /// <summary>
    /// Decides when a physical click has also been a firm press. Pure logic,
    /// no I/O.
    /// </summary>
    internal sealed class HapticsLatch
    {
        private const int CooldownMs = 250;

        private readonly int _threshold;
        private readonly int _release;
        private bool _armed;
        private int _above;
        private int _below;
        private long _last = -1000;

        public HapticsLatch(int threshold)
        {
            _threshold = threshold;
            // 70% hysteresis. Keep in sync with the label next to the
            // threshold slider (MainWindow.UpdateHapticsLabels).
            _release = threshold * 7 / 10;
        }

        public void Reset()
        {
            _armed = false;
            _above = 0;
            _below = 0;
        }

        public bool Sample(int pressure, bool valid, bool button, long ms)
        {
            if (!valid || pressure <= _release)
            {
                _above = 0;
                if (++_below >= 2)
                    _armed = true;
            }
            else
            {
                _below = 0;
                if (button && pressure >= _threshold)
                    _above++;
                else
                    _above = 0;
            }

            if (valid && _armed && _above >= 2 && ms - _last >= CooldownMs)
            {
                _armed = false;
                _above = 0;
                _last = ms;
                return true;
            }

            return false;
        }
    }

    /// <summary>
    /// The second haptic click, running inside the control panel: a
    /// background thread reads the driver's live pressure frames (shared with
    /// the Live view through DeviceIo.SetLiveConsumer) and, on a firm press
    /// after the physical click, fires one click through the actuator
    /// driver. It keeps running while the window is hidden in the tray and
    /// costs nothing while disabled: the live stream is released and the
    /// actuator closed.
    /// </summary>
    internal sealed class HapticsEngine : IDisposable
    {
        private const int PollMs = 8;
        private const int IdleMs = 250;
        private const int RetryMs = 500;
        private const int ActuatorSearchMs = 2000;
        private const int StaleMs = 250;

        private readonly DeviceIo _device;
        private readonly CancellationTokenSource _cts = new CancellationTokenSource();
        private readonly Thread _thread;

        // Shared with TestClick (UI thread); guards _actuator only.
        private readonly object _actuatorLock = new object();
        private ActuatorHandle? _actuator;
        private long _nextActuatorSearchMs;

        // Written by Configure (UI thread), read by the worker.
        private volatile HapticsConfig _config = new HapticsConfig { Enabled = false };

        private int _state = (int)HapticsState.Disabled;
        private int _pressure;
        private int _clicks;

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
            Volatile.Read(ref _clicks));

        /// <summary>Takes effect within one poll interval.</summary>
        public void Configure(HapticsConfig config)
        {
            _config = config.Clamped();
        }

        /// <summary>
        /// Fires one click now, saved settings or not, so a waveform can be
        /// judged before pressing Save. Works while the engine is disabled.
        /// </summary>
        public bool TestClick(int waveform)
        {
            lock (_actuatorLock)
            {
                if (_actuator == null)
                    OpenActuatorUnsafe();
                if (_actuator == null)
                    return false;

                return ClickUnsafe(waveform);
            }
        }

        private void Run()
        {
            var clock = Stopwatch.StartNew();
            CancellationToken token = _cts.Token;
            HapticsConfig applied = _config;
            var latch = new HapticsLatch(applied.Threshold);
            uint sequence = 0;
            long lastFrameMs = 0;
            bool liveHeld = false;

            try
            {
                while (!token.IsCancellationRequested)
                {
                    long now = clock.ElapsedMilliseconds;
                    HapticsConfig cfg = _config;

                    if (!cfg.Enabled)
                    {
                        // Nothing to do: let the driver stop assembling live
                        // frames and give the actuator back.
                        if (liveHeld)
                        {
                            _device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, false);
                            liveHeld = false;
                        }

                        CloseActuator();
                        Publish(HapticsState.Disabled, 0);
                        token.WaitHandle.WaitOne(IdleMs);
                        continue;
                    }

                    if (cfg.Threshold != applied.Threshold || cfg.Enabled != applied.Enabled)
                        latch = new HapticsLatch(cfg.Threshold);
                    applied = cfg;

                    if (!_device.IsConnected)
                    {
                        liveHeld = false;
                        Publish(HapticsState.DriverUnavailable, 0);
                        token.WaitHandle.WaitOne(RetryMs);
                        continue;
                    }

                    // No actuator, no point streaming frames.
                    if (!EnsureActuator(now))
                    {
                        if (liveHeld)
                        {
                            _device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, false);
                            liveHeld = false;
                        }

                        Publish(HapticsState.ActuatorUnavailable, 0);
                        token.WaitHandle.WaitOne(RetryMs);
                        continue;
                    }

                    // Cheap when nothing changed; re-sent after a reconnect or
                    // a failed read.
                    if (!liveHeld)
                    {
                        if (!_device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, true))
                        {
                            Publish(HapticsState.FeedUnavailable, 0);
                            token.WaitHandle.WaitOne(RetryMs);
                            continue;
                        }

                        liveHeld = true;
                        sequence = 0;
                        lastFrameMs = now;
                        latch.Reset();
                    }

                    if (!_device.TryGetLiveFrame(out LiveFrame frame))
                    {
                        liveHeld = false;
                        Publish(HapticsState.FeedUnavailable, 0);
                        token.WaitHandle.WaitOne(RetryMs);
                        continue;
                    }

                    if (frame.Sequence == sequence)
                    {
                        // No new frame. After a quiet spell forget any
                        // half-seen press so stale samples cannot combine
                        // with fresh ones.
                        if (now - lastFrameMs > StaleMs)
                        {
                            latch.Reset();
                            Volatile.Write(ref _pressure, 0);
                        }

                        token.WaitHandle.WaitOne(PollMs);
                        continue;
                    }

                    sequence = frame.Sequence;
                    lastFrameMs = now;

                    bool valid = frame.ContactCount == 1
                        && frame.RawContactCount == 1
                        && frame.LargePalmBlanked == 0
                        && frame.Contacts[0].Confident != 0
                        && frame.Contacts[0].PalmSuspect == 0;
                    int pressure = valid ? frame.Contacts[0].Pressure : 0;

                    Publish(HapticsState.Running, pressure);

                    if (latch.Sample(pressure, valid, frame.ButtonDown != 0, now) && TryClick(cfg.Waveform))
                        Interlocked.Increment(ref _clicks);

                    token.WaitHandle.WaitOne(PollMs);
                }
            }
            finally
            {
                if (liveHeld)
                    _device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, false);
                CloseActuator();
            }
        }

        private void Publish(HapticsState state, int pressure)
        {
            Volatile.Write(ref _state, (int)state);
            Volatile.Write(ref _pressure, pressure);
        }

        private bool EnsureActuator(long now)
        {
            lock (_actuatorLock)
            {
                if (_actuator != null)
                    return true;

                if (now < _nextActuatorSearchMs)
                    return false;

                OpenActuatorUnsafe();
                if (_actuator == null)
                    _nextActuatorSearchMs = now + ActuatorSearchMs;
                return _actuator != null;
            }
        }

        private void OpenActuatorUnsafe()
        {
            if (HapticsActuator.Find(out string? path) == HapticsActuator.FindResult.Found && path != null)
                _actuator = HapticsActuator.Open(path);
        }

        private void CloseActuator()
        {
            lock (_actuatorLock)
            {
                _actuator?.Dispose();
                _actuator = null;
            }
        }

        private bool TryClick(int waveform)
        {
            lock (_actuatorLock)
            {
                return _actuator != null && ClickUnsafe(waveform);
            }
        }

        // Returns true when a click was actually delivered. The driver's own
        // cooldown is flow control; any other failure means the transport is
        // gone or wedged, so it is dropped and found again later.
        private bool ClickUnsafe(int waveform)
        {
            int error = HapticsActuator.Click(_actuator!, waveform);
            if (error == 0)
                return true;

            if (error != HapticsActuator.ErrorBusy)
            {
                _actuator!.Dispose();
                _actuator = null;
            }

            return false;
        }

        public void Dispose()
        {
            _cts.Cancel();

            // Disposing the token source under a worker that is still waiting
            // on its handle would throw on that thread; if it did not stop in
            // time, leave the source to the finalizer instead.
            if (_thread.Join(1000))
                _cts.Dispose();
        }
    }
}
