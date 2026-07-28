# O'Louie Capture Performance Hardening Plan

## Objective

Keep recordings smooth and correctly timed when a game or other demanding
application is using most of the GPU, encoder, CPU, or storage bandwidth. Favor
bounded, observable degradation over hidden stalls, unbounded memory growth, or
cross-track data loss.

This file is the durable scope and sequencing reference for the work. Update the
status and verification notes as each phase is completed.

## Current Baseline

- Windows Graphics Capture runs free-threaded with a three-frame pool.
- Captured frames stay on the GPU through BGRA copy, NV12 conversion, and
  hardware H.264 encoding.
- Video uses an eight-frame bounded queue. On overload it discards the stale
  queued backlog and immediately admits the newest captured frame.
- A dedicated event-driven worker drains video independently; the recorder's
  10 ms service loop remains responsible for timely audio draining and fault
  supervision.
- Encoded packets enter a bounded asynchronous writer. Export snapshots use an
  ordered flush barrier, and close drains every accepted packet before recovery.
- Clip export runs on a below-normal-priority background worker.
- The configured FPS paces incoming WGC frames before app-owned texture copy.

## Phase 1 - Correct Frame Pacing and Encoder Sample Lifetime

Status: Complete

### Scope

1. Add a small, deterministic video frame cadence component that:
   - accepts a target FPS numerator and denominator;
   - uses capture timestamps in a declared frequency;
   - accepts the first frame immediately;
   - admits at most one frame per target interval;
   - advances from the actual accepted timestamp so delayed sources recover
     without emitting catch-up bursts;
   - reports accepted and rate-limited frame counts.
2. Apply cadence filtering in the WGC callback before allocating or copying the
   app-owned BGRA texture.
3. Pass the configured recording FPS into WGC session options. A disabled cadence
   remains available for smoke/fake callers that do not request pacing.
4. Separate intentionally rate-limited frames from downstream queue-overflow
   drops in capture snapshots and recorder diagnostics.
5. Release retained Media Foundation input samples as encoded output is drained,
   while preserving samples still owned by an in-flight asynchronous transform.
6. Add deterministic unit coverage for 60 FPS pacing against simulated 60, 120,
   144, and 240 FPS sources, delayed timestamps, invalid configuration, reset,
   and counters.
7. Add encoder sample-lifetime assertions where the current test boundary permits
   them without requiring a specific physical GPU or encoder vendor.

### Completion Criteria

- A configured 60 FPS recording copies no more than 60 eligible WGC frames per
  second, within integer timestamp rounding tolerance.
- Rate-limited frames do not count as queue-overflow drops.
- Delayed capture resumes from the newest eligible timestamp without a burst of
  historical catch-up work.
- Media Foundation input sample retention remains bounded during a recording.
- Normal and FFmpeg-enabled builds and complete test suites pass.

## Phase 2 - Resource Reuse and Overload Recovery

Status: Complete

### Scope

1. Pool app-owned BGRA capture textures with move-only leases. Allocate lazily,
   bound the pool to queue capacity plus capture/encode headroom, return textures
   automatically on every queue/drop/error path, and count creation, reuse,
   returns, exhaustion, and peak use.
2. Cache D3D11 VideoProcessor input views for the bounded capture-texture set and
   reuse the fixed NV12 output view. Bound the input-view cache and expose create,
   reuse, and eviction counts.
3. Add a `KeepNewest` overflow policy. When the video queue saturates, discard
   its stale backlog as one observable recovery event and admit the incoming
   newest frame instead of encoding a delayed queue after load subsides.
4. Propagate exact dropped-frame counts through the capture sink and encode
   bridge, including multi-frame backlog recovery.
5. Track current/peak queue depth, current/maximum oldest-frame age, overflow
   events, recovery events, last reason, and dropped frames by reason.
6. Surface queue, texture-pool, and converter-view telemetry in live diagnostics,
   copied diagnostics reports, and diagnostic logs.

### Completion Criteria

- The WGC callback does not allocate one new full-size BGRA texture per accepted
  frame during steady-state recording.
- Repeated conversion of the bounded capture-texture set reuses input views and
  the fixed encoder output view.
- Queue saturation cannot leave a full stale backlog to be encoded after the
  producer recovers; the newest incoming frame remains queued.
- Pool exhaustion and every overflow policy report exact dropped-frame counts.
- Normal and FFmpeg-enabled builds and complete test suites pass.

## Phase 3 - Decoupled Capture, Encoding, and Storage

Status: Complete

### Scope

1. Replace recorder-loop video polling with a dedicated worker signaled whenever
   WGC admits a frame. Drain bounded batches, keep exact queue/drop semantics,
   and stop only after the admitted queue is empty.
2. Keep video conversion, H.264 submission/output waits, and video packet handoff
   off the recorder's audio/supervision loop. Publish cached encode snapshots so
   low-frequency diagnostics cannot wait behind the encoder.
3. Give `PacketStore` one bounded asynchronous writer thread. Copy accepted
   packets into a 512-packet/32 MiB FIFO, reject overflow as a typed write fault,
   keep key/config/discontinuity boundaries durable, and serialize all file I/O
   through that worker.
4. Preserve export and recovery guarantees: an export snapshot inserts an
   ordered flush barrier; close drains all accepted packets, flushes, and joins;
   recovery continues accepting only the complete on-disk packet prefix.
5. Measure WGC texture-copy submission, queue wait, VideoProcessor conversion
   submission, Media Foundation encoder-output wait, packet queue wait, and
   packet write latency. Expose current/peak queue pressure and last/maximum/
   average stage latency through diagnostics reports and logs.
6. Add deterministic coverage for event-worker lifecycle/wakeup/drain behavior,
   asynchronous export barriers, close draining, writer telemetry, invalid
   bounds, and explicit queue-overflow failure.

### Completion Criteria

- Video conversion/encoding never executes on the recorder's audio service loop.
- Diagnostics reads do not acquire the video processing lock.
- No encoder thread performs file I/O, and writer pressure remains bounded and
  observable instead of silently dropping an encoded track packet.
- Active export sees every packet accepted before its flush barrier; normal close
  makes every accepted packet recoverable.
- Normal and FFmpeg-enabled builds and complete test suites pass.

## Phase 4 - Capture-First Scheduling

Status: Complete

- Register capture and encode workers with appropriate Windows multimedia
  scheduling.
- Add a conservative opt-in Capture First mode without realtime process priority.
- Probe hardware encoder low-latency and quality-versus-speed controls and report
  which settings were actually accepted.

## Phase 5 - Video Settings and Recording Health

Status: Not started

- Replace ambiguous quality presets with workload-oriented profiles.
- Make target FPS behavior and output-resolution cost clear in the Video UI.
- Add Balanced and Capture First priority choices.
- Add automatic overload protection and a concise recording-health state that
  distinguishes capture, encoder, and storage pressure.

## Verification Record

### Phase 1 - 2026-07-20

- Default Debug CMake build: passed.
- Default Debug CTest suite: 6/6 passed.
- FFmpeg-enabled Debug CMake build: passed against `_deps/ffmpeg`.
- FFmpeg-enabled Debug CTest suite: 6/6 passed.
- Deterministic cadence tests: 60 accepted frames over one second from simulated
  60, 120, 144, and 240 FPS timestamp sources; delayed-source rebase, invalid
  configuration, non-monotonic timestamp, reset, and counter behavior passed.
- Real hardware path: `O'LouieEncodeSmoke --h264-bgra-submit 1280 720 60 10`
  succeeded through the NVIDIA H.264 Encoder MFT. Three submitted BGRA frames
  produced three packets and the retained input-sample count returned to zero
  after every available-output drain.
- A full production recording was not started during this phase. The cadence
  decision logic is deterministic-tested and is wired into the production WGC
  callback before app-owned texture allocation.

### Phase 2 - 2026-07-20

- Default Debug CMake build: passed.
- Default Debug CTest suite: 6/6 passed.
- FFmpeg-enabled Debug CMake build: passed against `_deps/ffmpeg`.
- FFmpeg-enabled Debug CTest suite: 6/6 passed.
- Deterministic GPU-backed tests verified lazy two-texture pool creation, bounded
  exhaustion, move-only lease return, exact lifecycle counters, and reuse of the
  same returned D3D11 texture.
- Queue tests verified exact drop counts and telemetry for drop-newest,
  drop-oldest, and freshest-frame recovery. A three-frame stale backlog was
  discarded as one recovery event and the incoming newest timestamp remained.
- Real GPU conversion smoke succeeded on an NVIDIA GeForce RTX 3060 and verified
  reuse of both the VideoProcessor input view and fixed NV12 output view.
- Real hardware `O'LouieEncodeSmoke --h264-bgra-submit 1280 720 60 10`
  succeeded through the NVIDIA H.264 Encoder MFT; three BGRA frames produced
  three encoded packets.
- A full production recording was not started. The production WGC session is
  wired to a queue-capacity-plus-two texture pool, and pool/queue/view behavior
  is covered at the deterministic boundaries without requiring a microphone.

### Phase 3 - 2026-07-21

- Default Debug CMake build: passed.
- Default Debug CTest suite: 6/6 passed.
- FFmpeg-enabled Debug CMake build: passed against `_deps/ffmpeg` (only the
  existing conversion warnings inside FFmpeg headers were emitted).
- FFmpeg-enabled Debug CTest suite: 6/6 passed.
- Deterministic encode tests verified event-driven start/repeated-start, captured
  frame notification, bounded batch wake/drain, telemetry publication, stop and
  repeated-stop behavior using an injected backend boundary.
- Deterministic record tests queued 128 packets through the asynchronous writer,
  verified an active-export flush barrier at packet 64, verified close-drain and
  recovery of all 128 packets, checked writer queue/latency counters, and proved
  invalid limits and bounded-queue overflow fail explicitly.
- Real hardware `O'LouieEncodeSmoke --h264-bgra-submit 1280 720 60 10`
  succeeded on an NVIDIA GeForce RTX 3060 through the NVIDIA H.264 Encoder MFT;
  three submitted frames produced three recoverable H.264 packets through the
  asynchronous `PacketStore`.
- A full production recording was not started. The production WGC sink is wired
  to the event-driven worker, and the normal recorder still services direct
  system/microphone audio independently; microphone hardware was not required
  for this phase.

### Phase 4 - 2026-07-21

- Default Debug CMake build: passed.
- Default Debug CTest suite: 6/6 passed.
- FFmpeg-enabled Debug CMake build: passed against `_deps/ffmpeg`.
- FFmpeg-enabled Debug CTest suite: 6/6 passed.
- Recorder/audio-service, WGC callback, live WASAPI source, and video-encode
  work register with Windows MMCSS (`Capture` for recorder/video work and
  `Audio` for WASAPI). Balanced requests normal relative priority; Capture
  First requests high relative priority. Registration is best-effort,
  thread-scoped, reverted on exit, and never changes process priority.
- Settings schema version 3 persists Balanced or Capture First. Version-1 and
  version-2 files migrate in memory to Balanced, unsupported newer files remain
  protected, and the Video tab explains the speed/quality/game-FPS tradeoff.
- Deterministic tests cover both scheduling plans, invalid policies, RAII reset,
  Capture First pipeline propagation, real-time-process rejection, encoder
  tuning plans, event-worker scheduling telemetry, settings migration, and
  diagnostic reporting.
- Real NVIDIA GeForce RTX 3060 / NVIDIA H.264 Encoder MFT smoke ran
  `--h264-bgra-submit 1280 720 60 10 --capture-first`. The transform accepted
  and read back low-latency mode `1` and quality-versus-speed `0`; all three
  BGRA frames produced three recoverable H.264 packets.
- A full production recording under game load was not started. No microphone
  was needed for this phase; broader workload profiles and automatic recording
  health remain Phase 5.
