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
        // Enabled and the actuator is present; the live pressure meter is not
        // being fed (it is only streamed while the Live switch is on).
        Idle,
        // Enabled, actuator present, live pressure meter being fed.
        Running,
    }

    internal readonly record struct HapticsStatus(HapticsState State, int Pressure, int Clicks);

    /// <summary>
    /// Companion of the second haptic click inside the control panel. The
    /// click itself is played by the touchpad driver (it works without this
    /// app); this class only (1) finds the actuator driver so the UI can say
    /// whether it is installed, (2) plays the "Test click", and (3) while the
    /// live pressure meter is visible (MeterWanted), reads the driver's live
    /// pressure frames (shared with the Live view through
    /// DeviceIo.SetLiveConsumer). It asks the driver for nothing otherwise.
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

        /// <summary>
        /// Set by the UI while the live pressure meter is visible. Only then
        /// is the driver asked for live frames.
        /// </summary>
        public volatile bool MeterWanted;

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

                    if (!_device.IsConnected)
                    {
                        liveHeld = false;
                        Publish(HapticsState.DriverUnavailable, 0);
                        token.WaitHandle.WaitOne(RetryMs);
                        continue;
                    }

                    // The second click itself is played by the touchpad driver
                    // (it works without this app). All that is left here is to
                    // tell the user whether the actuator driver is present and
                    // to feed the live pressure meter while it is visible.
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

                    // No meter wanted: do not make the driver assemble live frames.
                    if (!MeterWanted)
                    {
                        if (liveHeld)
                        {
                            _device.SetLiveConsumer(DeviceIo.LiveConsumer.Haptics, false);
                            liveHeld = false;
                        }

                        Publish(HapticsState.Idle, 0);
                        token.WaitHandle.WaitOne(IdleMs);
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
