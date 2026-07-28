# Capture Boundary

This folder will own monitor capture sources.

Primary path: Windows Graphics Capture using `IGraphicsCaptureItemInterop::CreateForMonitor(HMONITOR)` and a free-threaded `Direct3D11CaptureFramePool`.

Fallback path: DXGI Desktop Duplication, including rotation and pointer composition.

The capture callback must stay light: timestamp, copy or enqueue bounded app-owned resources, release the frame, and return.

Current implemented pieces:

- `WgcMonitorCapture`: manual smoke path that creates a WGC monitor item, starts a free-threaded frame pool, counts frames/timestamps briefly, and stops.
- `VideoFrameCadence`: deterministic rational-timestamp pacing for a configured output FPS. It accepts one freshest frame per output slot, tolerates one capture-clock tick of integer rounding, rebases after delayed delivery without a catch-up burst, and reports accepted/rate-limited/invalid counts.
- `BgraTexturePool`: lazily creates a bounded set of app-owned BGRA D3D11 textures and returns them through move-only leases. It reports allocation, reuse, return, exhaustion, failure, and peak-use telemetry.
- `WgcMonitorCaptureSession`: stoppable long-running monitor capture owner used by the tray recorder. Production recording configures it with the selected target FPS and a queue-capacity-plus-two BGRA texture pool; it drains delayed frame-pool batches to the freshest surface, rate-limits before texture acquisition/copy, counts pool exhaustion as an explicit drop, dispatches accepted textures through a caller-owned bounded sink, applies thread-scoped `Capture` MMCSS scheduling while callbacks execute, and waits for active callbacks during stop.
- `VideoFrameQueue`: thread-safe bounded owned-frame handoff queue for app-owned D3D11 textures, dimensions, and timestamps. Besides drop-newest/drop-oldest, production uses `KeepNewest` to discard a saturated stale backlog and admit the incoming newest frame. Statistics include exact drops by reason, overflow/recovery events, current/peak depth, and current/maximum oldest-frame age.
- `CapturedVideoFrameSink`: defines the captured app-owned BGRA frame sink/dispatch boundary used by WGC frame-copy handoff. Frames are app-owned D3D11 textures and may be moved into caller-owned queue/encode state.
- `RunWgcMonitorFrameCopySmoke`: bounded manual frame-copy smoke that copies WGC surfaces into app-owned BGRA textures inside the callback, releases WGC frames immediately, either pushes into an internal `VideoFrameQueue` for legacy vector-return smoke paths or dispatches to a caller-owned `ICapturedVideoFrameSink`, and reports copied/dropped frame counts.

Still deferred here:

- DXGI Desktop Duplication fallback.
- Seamless resize recreation and device-lost recovery; the current recorder fails explicitly if monitor dimensions change during a recording.
