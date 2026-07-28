# O'Louie Progress

This file tracks implementation passes against `clip_recorder_implementation_plan.txt`. It is intentionally short so each pass has a clear landing point.

Development setup is Codex-owned. Codex may automatically download, install, update, configure, and use FFmpeg or any other local development and verification dependency needed to advance the project; the user is not expected to perform routine setup. Licensing, provenance, dynamic-linking, and shipping constraints still apply to what may become part of the distributed product.

## Milestone Status

1. Shell: complete.
   - CMake project, Win32 entrypoint, app host, single-instance guard, runtime paths, logger, hidden message window, tray shell, hotkey shell, settings defaults, and smoke tests are in place.

2. D3D/WGC capture smoke: complete.
   - Monitor enumeration is implemented.
   - D3D11 device creation for the selected monitor adapter is implemented.
   - BGRA-to-NV12 planning validates source/output dimensions and even NV12 output sizing.
   - GPU texture allocation smoke creates app-owned BGRA source and NV12 output textures on the selected adapter without CPU readback.
   - D3D11 VideoProcessor smoke executes a BGRA-to-NV12 blit into an NV12 texture on the selected adapter without CPU readback.
   - `GpuBgraToNv12Converter` owns reusable D3D11 VideoProcessor setup for a planned BGRA/NV12 size, caches bounded input views, reuses the fixed encoder output view, and reports view lifecycle counters through a narrow `Convert` boundary.
   - Manual WGC monitor-capture smoke counts frames/timestamps without recording or encoding.
   - Bounded WGC frame-copy smoke copies monitor frames into app-owned BGRA textures, releases WGC frames immediately, and reports copied/dropped frame counts for manual encode smoke paths.
   - `BgraTexturePool` supplies lazily allocated, bounded app-owned BGRA textures through move-only leases with allocation/reuse/return/exhaustion/peak stats.
   - `VideoFrameQueue` owns the bounded leased-texture handoff between capture and video encode work, including drop-newest/drop-oldest policies plus production freshest-frame recovery, exact multi-frame drop reporting, FIFO drain/pop, invalid-frame rejection, current/peak depth, oldest-frame age, and overflow-reason stats.
   - Automated verification covers monitor enumeration, D3D11 device creation, and the capture-exclusion helper on a temporary top-level window.
   - The actual WGC frame-count smoke remains manual so normal builds do not trigger screen-capture consent prompts.

3. PacketStore + fake mux tests: complete.
   - Timebase conversions are implemented and tested.
   - Encoded packet metadata and binary `packets.dat` append storage are implemented.
   - PacketStore keyframe-aligned range queries are implemented and tested.
   - PacketStore recovery by scanning `packets.dat` is implemented and tested.
   - Bookmark data model and export range calculation are implemented and tested.
   - Muxer-facing packet plan construction is implemented and tested.
   - `Mp4Muxer` request validation, H.264 plus direct raw-AAC metadata handoff, per-track packet payload/timestamp dry runs, explicit FFmpeg backend availability reporting, mixed stream setup validation, configured H.264/AAC interleaved packet writing, backend-unavailable behavior, FFmpeg runtime DLL staging, trailer finalization, and atomic rename are implemented and tested against the configured local FFmpeg root.
   - Bounded background active-clip export queueing is implemented; bookmark export orchestration remains in progress under Milestone 7.
4. Audio: complete for the full-recording gate.
   - Active render/capture endpoint enumeration is implemented and tested.
   - Default render/capture endpoint discovery is implemented and tested when available.
   - Live microphone check is implemented with asynchronous event-driven shared-mode WASAPI capture/playback, a bounded PCM FIFO, Windows audio-engine format conversion, saved output selection with default-device fallback, device-invalidation recovery, a dBFS/clipping meter, and recording mutual exclusion.
   - `O'LouieAudioSmoke` prints current render/capture endpoints without recording audio.
   - System loopback smoke reads PCM packet/frame counts and timestamps without encoding or persistence.
   - Microphone capture smoke reads PCM packet/frame counts and timestamps without encoding or persistence.
   - Shared PCM format, packet timing, and capture stats structures are implemented and tested.
   - Audio-to-PacketStore track planning is implemented and tested for default mixed, system loopback, mic, and future process-loopback tracks.
   - AAC encoder config validation and the backend-unavailable handoff boundary are implemented and tested.
   - Same-sample-rate PCM preparation for AAC input is implemented and tested for float32-to-signed-16 conversion and signed-16 pass-through.
   - Media Foundation AAC encoder discovery, activation, and input/output media-type initialization are implemented behind `AacEncoder`; setup now preserves the exact raw-AAC decoder configuration from `MF_MT_USER_DATA` plus sample rate, channels, bitrate, frame size, payload/profile/object type, and encoder identity. Normal automated tests still avoid host-specific transform execution.
   - AAC packet/result types plus `SubmitPcm`, `DrainAvailable`, and `Flush` are implemented and tested without PacketStore writes.
   - AAC-to-`PacketStore` metadata mapping and append handoff are implemented and tested without live capture threading.
   - `AacEncodeSink` synchronously accepts prepared PCM blocks, submits to an AAC encoder interface, drains packets, and appends them to `PacketStore`.
   - `PreparedPcmQueue` provides a bounded prepared-PCM handoff queue with reject-newest and drop-oldest overflow policies, FIFO pop, clear, and stats.
   - `AudioEncodeWorker` synchronously drains prepared PCM queue blocks into `AacEncodeSink` with per-call budgeting and sink-failure stop behavior.
   - `CapturedPcmQueueWriter` prepares captured PCM packets into queued AAC-ready PCM blocks, including zero-filled silent packets, and rejects packets whose actual format does not match the configured track input format.
   - `AudioTrackEncodeChain` owns the synchronous per-track composition of captured PCM staging, bounded queueing, AAC sink handoff, track-specific captured input format validation, and queue draining.
   - `AudioEncodeSession` owns multiple per-track encode chains and routes queue/drain/flush operations by AAC track id, including fair bounded all-track draining for future encode ticks.
   - `AudioEncodeSessionBinding` derives and validates session track bindings from audio track plans, per-track captured PCM format slots, queue options, and caller-provided encoder slots.
   - `AudioCaptureEncodeSetup` preflights direct WASAPI capture formats, builds direct-source AAC track plans and format slots, initializes real or injected AAC encoders, requires complete MP4-facing output metadata, and assembles caller-owned `PacketStore` sessions for capture encode paths while preserving explicit unsupported-format/backend failures.
   - `AudioRecordingMetadata` converts prepared direct-source plans and AAC encoder output state into validated per-track session-manifest metadata with stable source identity and exact decoder configuration.
   - `AudioSource` centralizes captured audio source identity validation for system loopback, microphone, and process-loopback sources.
   - `AudioSourceRouter` maps captured source identities to direct AAC source tracks from `AudioTrackPlan` and rejects unknown, disabled, or mixer-output sources.
   - `AudioSourceSessionDispatcher` resolves captured source identities through `AudioSourceRouter` and queues PCM plus its actual captured format into `AudioEncodeSession` by AAC track id while preserving separate route and queue failure statuses.
   - `CapturedPcmSink` defines a non-owning captured PCM packet view plus sink dispatch helper; loopback and microphone smoke paths can optionally hand captured packets to caller-owned sinks before WASAPI buffers are released.
   - `CapturedPcmSessionSink` implements `ICapturedPcmSink` by forwarding validated captured PCM packets into `AudioSourceSessionDispatcher`, preserving invalid-config, route, and queue failures distinctly.
   - `AudioCaptureEncodeBridge` composes source routing, source-to-session dispatch, captured PCM sink handoff, and bounded `AudioEncodeSession` draining around a caller-owned encode session.
   - `AudioCaptureManager` validates planned capture sources, attaches direct source tracks to a caller-owned `ICapturedPcmSink`, and marks process-loopback and default-mixed work as deferred instead of pretending to start them.
   - `AudioCaptureSmoke` runs supported `AudioCaptureManager` source bindings through duration-bounded default loopback/mic smoke paths and a caller-owned sink; `O'LouieAudioSmoke --capture-sources` exposes it manually.
   - `WasapiCaptureSource` owns live event-driven start/stop lifetime for one supported default WASAPI source, currently system loopback or microphone, and pushes PCM packets into a caller-owned `ICapturedPcmSink` without owning recorder, encoder, mux, or export state.
   - `AudioCaptureEncodeSmoke` composes duration-bounded source smoke with `AudioCaptureEncodeBridge` and drains a caller-owned `AudioEncodeSession` afterward; automated coverage uses fake source runners and fake AAC encoders.
   - `AudioLiveCaptureEncodeSession` exposes explicit long-running prepare/start/tick/stop/drain/flush ownership; the retained duration-bounded wrapper uses the same lifecycle. It serializes PCM sink/drain access across WASAPI threads and cleans up already-started sources when a later source fails.
   - `AudioRecordingSession` owns the recorder-facing audio-only preflight, AAC session setup, live capture/encode run options, and duration-bounded run orchestration around a caller-owned `PacketStore`; automated coverage uses fake WASAPI formats, fake AAC encoders, and fake live capture sources.
   - `O'LouieAudioSmoke --capture-encode` creates a temporary `PacketStore`, queries default WASAPI formats, initializes real Media Foundation AAC encoders for currently supported direct source tracks, runs `AudioCaptureEncodeSmoke`, flushes the encode session, and reports the packet-store output path.
   - `O'LouieAudioSmoke --live-capture-encode` creates a temporary `PacketStore`, queries default WASAPI formats, initializes real Media Foundation AAC encoders for direct source tracks, starts/stops live sources through `AudioLiveCaptureEncode`, flushes, and reports the packet-store output path.
   - `O'LouieAudioSmoke --audio-session` exercises `AudioRecordingSession` with real default WASAPI sources, real Media Foundation AAC encoders, a temporary caller-owned `PacketStore`, live draining, flush, and packet-store reporting.
   - Production cross-media ownership, shared video/audio timing, long-running source control, and ordered AAC finalization are complete. Process-loopback capture, PCM sample-rate conversion, and default mixed-track generation remain deferred.
5. Hardware video encoder: complete for the MVP video-only path.
   - `MfHardwareH264EncoderProbe` validates generic MVP H.264 requests, rejects CPU-fallback-adjacent invalid settings such as odd NV12 dimensions or B-frames, enumerates Media Foundation hardware H.264 encoder MFTs for NV12 input and H.264 output, records backend names/CLSIDs/enumeration flags, and fails explicitly when Media Foundation or hardware encoder discovery is unavailable.
   - `O'LouieEncodeSmoke` manually exercises the real hardware H.264 MFT probe without activating an encoder transform, submitting frames, writing packets, or saving video.
   - `MfHardwareH264EncoderSession` activates the selected hardware H.264 MFT, derives an NV12/H.264 media-type plan from validated settings, attaches an `IMFDXGIDeviceManager` when a caller supplies a D3D11 device and the transform is D3D11-aware, applies basic codec settings where supported, configures H.264 output and NV12 input media types, records D3D11/asynchronous traits, unlocks async transforms when required, can submit generated NV12 DXGI input samples for smoke verification, can drain available encoded output between submissions, inspects Annex B/SPS/PPS/IDR presence in drained packets, captures SPS/PPS payload bytes into a config record, derives AVCC extradata bytes for future MP4 metadata, maps drained packets into `PacketStore` H.264 packet metadata, and tears down cleanly.
   - `SyntheticVideoRecordingSession` owns the recorder-facing synthetic video handoff around a configured H.264 encoder session, caller-owned D3D11 device, and caller-owned `PacketStore`, including track validation, generated-frame timing submission, available-output draining, config capture, packet append, stats, and explicit stage failures.
   - `MfHardwareH264EncoderSession` can submit caller-owned NV12 textures as MF DXGI samples.
   - `BgraVideoRecordingSession` owns the encoder-facing caller-owned BGRA texture handoff, reusable BGRA-to-NV12 converter, reusable encoder-sized NV12 texture, caller-owned NV12 submit, available-output draining, H.264 config capture, and PacketStore append.
   - `VideoEncodeWorker` synchronously drains `VideoFrameQueue` frames into `BgraVideoRecordingSession` with a per-call frame budget, `Timebase`-based timestamp normalization, first session-failure preservation, and processed/dropped/remaining frame reporting.
   - `VideoEncodeChain` owns the synchronous video queue/session/worker composition with explicit queue options, BGRA session options, worker timing options, and default drain budget for future encode-thread scheduling.
   - `CapturedVideoFrameSink` defines the captured app-owned BGRA video-frame sink boundary used by WGC frame-copy handoff.
   - `VideoCaptureEncodeBridge` implements the captured-frame sink by forwarding app-owned BGRA frames into `VideoEncodeChain`, setting the chain timebase from the first captured frame when requested, and exposing bounded chain draining for caller-owned scheduling.
   - `VideoLiveCaptureEncode` composes WGC frame-copy handoff, `VideoCaptureEncodeBridge`, serialized sink/drain access, repeated bounded drain ticks while capture runs, and final draining into a caller-owned `VideoEncodeChain`.
   - `VideoRecordingSetup` owns the recorder-facing video preflight/setup boundary for the current video-only path: it validates source dimensions, H.264 config, queue capacity, drain budget, timing, `PacketStore` track id, and live runner options before building the H.264 track definition, `VideoEncodeChainConfig`, and live WGC encode options, then prepares a chain against caller-owned encoder, D3D11 device/context, and `PacketStore`.
   - `VideoRecordingRunSession` owns the current video-only recording run boundary: it composes video preflight, caller-owned encoder/D3D/`PacketStore` prepare, duration-bounded live WGC capture/encode run orchestration, run state, last live result, and explicit preflight/setup/run status mapping.
   - `VideoRecordingBootstrap` owns the current video-only runtime bootstrap: it resolves the requested or primary monitor, creates the matching D3D11 device/context, initializes the Media Foundation hardware H.264 session, creates the temporary H.264 `PacketStore`, prepares `VideoRecordingRunSession`, and preserves explicit bootstrap/preflight/setup/encoder/store failure statuses.
   - `VideoRecordingMetadata` builds validated video session metadata from explicit inputs or from a prepared `VideoRecordingBootstrapSession`, including PacketStore paths, video track id, H.264 SPS/PPS/AVCC config, requested encoder config, encoder/backend traits, monitor details, source dimensions, and output dimensions.
   - `SessionManifest` writes current version 3 metadata for the H.264 track, zero or more direct AAC tracks, and validated bookmarks to `session.json` beside `packets.dat`; version 1 video-only and version 2 H.264/AAC sessions remain readable, and missing, duplicate, codec-mismatched, or unsupported metadata is rejected explicitly.
   - `VideoExportPlan` builds a cross-media recovery/export input from a ready `SessionManifest` plus an active or recovered `PacketStore` snapshot, validates manifest/store paths and every packet-bearing current manifest track, requires keyframe-aligned video starts, preserves real late-audio offsets, reports configured AAC tracks omitted for an empty selected range, and produces H.264/AAC stream metadata plus one normalized interleaved `MuxPlan`.
   - `BuildVideoMp4MuxRequest` adapts `VideoExportPlan` into the `Mp4Muxer` request boundary with packet file path, H.264 AVCC metadata, per-track raw-AAC decoder configuration and format metadata, and matching mux-plan validation while leaving writer availability controlled by the explicit FFmpeg build configuration.
   - `Mp4Muxer::DryRunPayloadRead` walks a validated mixed MP4 request, reads every referenced payload from `packets.dat`, verifies per-track DTS, and reports separate video/audio packet and byte totals without writing MP4 output.
   - `Mp4Muxer::BackendAvailability` reports whether the LGPL dynamic FFmpeg backend was configured at build time, lists the expected dynamic libraries, and keeps `WriteMp4` explicitly backend-unavailable when FFmpeg is not configured.
   - `Mp4Muxer::ValidateStreamSetup` validates allocation of an FFmpeg MP4 output context, one H.264 video stream, every requested AAC audio stream, AVCC/`AudioSpecificConfig` handoff, and reported stream facts when `OLOUIE_ENABLE_FFMPEG=ON`, without writing packet payloads or files. `ValidateVideoStreamSetup` forwards to it for compatibility.
   - `Mp4Muxer::WriteMp4` now writes configured H.264 plus direct AAC MP4 output: it converts PacketStore Annex B H.264 payloads into MP4 length-prefixed samples, passes raw AAC access units to their streams, interleaves by DTS through FFmpeg, writes header/trailer to a temporary file, and atomically renames to the final output.
   - `O'LouieEncodeSmoke --h264-session` manually creates a primary-monitor D3D11 device, exercises the real transform activation/configuration/DXGI-manager path, and reports accepted/rejected codec settings.
   - `O'LouieEncodeSmoke --h264-submit` manually creates generated NV12 D3D11 textures, wraps them as MF DXGI samples, starts the transform stream, routes a short monotonic frame sequence through `SyntheticVideoRecordingSession`, drains available output between submissions, reports encoded packet count, bytes, keyframe flag, PTS, duration, packet format, NAL count, SPS/PPS count, SPS/PPS payload byte sizes, AVCC extradata byte size, IDR count, and whether MP4 extradata can be derived, then verifies the temporary `PacketStore` output.
   - `O'LouieEncodeSmoke --h264-bgra-submit` manually creates generated BGRA D3D11 textures, routes them through `BgraVideoRecordingSession`, converts them to NV12 on GPU, submits caller-owned NV12 textures to the hardware H.264 MFT, drains encoded packets, captures AVCC config, and verifies temporary `PacketStore` output.
   - `O'LouieEncodeSmoke --h264-wgc-submit` builds the video runtime through `VideoRecordingBootstrap`, then runs the prepared `VideoRecordingRunSession`, captures a bounded batch of real WGC monitor frames into app-owned BGRA textures, dispatches them through `VideoLiveCaptureEncode`/`VideoCaptureEncodeBridge` into `VideoEncodeChain`, repeatedly drains while capture runs, converts frames to NV12 on GPU, submits caller-owned NV12 textures to the hardware H.264 MFT, drains encoded packets, captures AVCC config, verifies temporary `PacketStore` output, builds ready video metadata, writes `session.json`, reads the manifest back, closes the store, recovers `packets.dat`, builds a video-only export plan, validates a video MP4 mux request shape, reports FFmpeg backend availability, validates FFmpeg stream setup when configured, dry-runs payload reads, and attempts configured-only video MP4 writing.
   - The long-running path now final-drains the hardware transform after queued frames are submitted, appends remaining packets, and preserves final H.264 config before session metadata is written.
6. End-to-end recording: complete for system audio and optional direct microphone tracks.
   - `WgcMonitorCaptureSession` owns explicit start/stop lifetime for a free-threaded WGC monitor capture, copies frames into bounded app-owned BGRA resources, reports capture stats/errors, and waits for active callbacks during stop.
   - `VideoRecorderPipeline` preflights requested WASAPI formats plus H.264/AAC encoders, creates one H.264/AAC `PacketStore`, establishes one QPC-derived 100 ns origin, starts direct audio before WGC, drains both media families during recording, stops both inputs, drains queued video/PCM, finalizes H.264, flushes AAC, writes cross-media metadata, recovers the store, and atomically saves one interleaved MP4.
   - `VideoRecorderSession` runs that pipeline asynchronously with explicit idle/starting/recording/stopping/saved/failed state, repeated-command rejection, start cancellation, worker exception containment, and a test-injectable runner.
   - `AppHost` maps the existing `Ctrl+Shift+R` hotkey and dynamic tray command to the same start/stop action, marshals worker state back through the hidden window, and reports preparing, recording, saving, saved path, and failure states.
   - The normal settings mapping enables system loopback by default, carries optional microphone enablement, keeps default mixing deferred, and leaves no-FFmpeg start failure ahead of capture.
7. Clips/bookmarks: complete for the MVP defaults.
   - Live `PacketStore` export snapshots flush writer visibility and copy immutable index data without stopping capture.
   - The 30-second and 5-minute clip presets clamp to available time, align to the previous H.264 keyframe, include overlapping AAC, and mux on one bounded low-priority background queue.
   - `Ctrl+Shift+F8`, `Ctrl+Shift+F9`, and their recording-only tray commands report queued/saved/failed while the primary recording remains active.
   - `Ctrl+Shift+F10` and **Add Bookmark and Save Clip** create a recorder-owned marker at the current encoded-video end, export the default 60-second pre-roll/zero-post-roll window through the same bounded path, and preserve bookmark metadata in session manifest version 3.
   - Nonzero bookmark post-roll remains explicitly deferred until future-packet scheduling is exposed with settings.
8. Settings UI: complete.
   - Versioned atomic settings persistence, version-1/version-2-to-3 migration, safe fallback diagnostics, canonical configurable hotkeys with transactional registration rollback, start-with-Windows synchronization, selected-monitor/output/video/audio/clip/performance settings application, saved mic-check output selection, live microphone level/status UI, pinned Dear ImGui Win32/D3D11 integration, capture exclusion, visible-only capped rendering, all six current settings/diagnostics tabs, focused tests, visual lifecycle proof, and real configured settings-to-recording/export runs are complete.
9. Robustness: in progress.
   - Configured audio sources with no AAC packets in a selected range no longer block clip, bookmark, or full export. The manifest retains configured-source metadata while each MP4 includes only packet-bearing AAC streams and never fabricates silence.
   - Selected-monitor resize/disconnect is latched explicitly from WGC content, capture-item closure, and selected-monitor identity/rectangle polling. It stops the fixed-dimension stream once, drains/finalizes recoverable data, reports the saved path, and never switches monitors or encoders silently.
   - D3D11 removed/reset/hung and hardware H.264 runtime failures are typed across WGC copy, GPU conversion, DXGI sample submission, and transform drain/finalization. They stop once, discard queued dead-device textures, preserve already encoded packets where possible, name the failed subsystem, and never fall back to CPU video encoding.
   - PacketStore, manifest, and MP4 disk/write failures are typed by subsystem, operation, path, and cause. Unsafe live writes stop once and proceed through bounded preservation finalization; incomplete temps are cleaned while complete unpublished artifacts and recoverable session data remain available.
   - Startup recovery runs through one bounded worker, classifies durable and temporary session artifacts, reports actionable sessions through the tray, protects existing outputs, and supports retryable export plus reversible discard.
   - The low-frequency diagnostics panel, bounded long-session exercise tooling, audio backpressure hardening, configured export-under-game-load preflights, graceful external-stop handling, and strict long-session evidence checker are complete. A user-bounded 26-minute run also recovered cleanly after interrupted final publication; the explicit longer-than-one-hour run remains.

## Current Pass

Pass 154 completed Phase 4 of capture-performance hardening:

- The recorder/audio-service worker, WGC frame callbacks, live WASAPI source
  threads, and dedicated video encode worker now use best-effort, thread-scoped
  Windows MMCSS scheduling. Video work uses `Capture`, audio sources use
  `Audio`, Balanced uses normal relative priority, and Capture First uses high.
- Settings schema version 3 and the Video tab add a persisted Balanced/Capture
  First choice. Version-1 and version-2 settings migrate to Balanced, and no
  path raises the process to real-time priority.
- Capture First asks `ICodecAPI` for low-latency mode and the fastest
  quality-versus-speed value. Support, modifiability, application, readback,
  accepted value, and driver messages flow into diagnostics and logs.
- Deterministic scheduling, encoder-policy, settings, pipeline, worker, and
  diagnostics tests pass. Default and FFmpeg-enabled Debug builds and suites
  each passed 6/6.
- Real NVIDIA GeForce RTX 3060 / NVIDIA H.264 Encoder MFT smoke accepted and
  read back low-latency `1` and quality-versus-speed `0`, then encoded and
  recovered all three generated 1280x720 BGRA frames.

## Previous Pass

Pass 153 completed event-driven video encoding, bounded asynchronous packet
storage, and stage latency telemetry in Phase 3.

## Next Pass

Proceed to Phase 5 from `docs/capture_performance_plan.md` when requested:

- Replace ambiguous video quality presets with workload-oriented profiles.
- Clarify target-FPS and output-resolution costs in Video settings.
- Add automatic overload protection and a concise recording-health state that
  distinguishes capture, encoder, and storage pressure.
- The explicit longer-than-one-hour configured recording criterion remains open
  until the user is ready to run it with a microphone connected.
