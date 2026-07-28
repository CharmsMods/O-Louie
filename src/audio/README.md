# Audio Boundary

This folder will own WASAPI capture and audio encoding input preparation:

- System output loopback.
- Microphone capture.
- Process-tree application loopback when the OS supports it.
- Timestamping, drift handling, resampling, and AAC encoder handoff.
- Mapping enabled audio sources to future AAC PacketStore tracks.

Audio capture must not block the UI, capture callback, video encoder, or export queue.

Current implemented pieces:

- `AudioEndpointManager`: enumerates active render/capture endpoints and discovers default render/capture endpoints without starting capture.
- `MicMonitorSession`: asynchronously monitors the current Windows default microphone through Windows Default or a selected stable render-endpoint ID. Its event-driven shared-mode WASAPI backend uses the Windows audio engine for format/rate conversion, keeps a bounded quarter-second PCM FIFO, reports level/clipping and queue pressure through thread-safe snapshots, retries an invalidated selected output through Windows Default, and never records or encodes the monitored audio.
- `AudioResampler`: prepares same-sample-rate PCM for AAC by converting float32 to signed 16-bit PCM or copying already prepared signed 16-bit PCM. Real sample-rate conversion is not wired yet.
- `CapturedPcmQueueWriter`: turns captured PCM packet timing plus same-rate PCM bytes into prepared queue blocks, including zero-filled blocks for silent capture packets, after confirming the packet format matches the configured track input format.
- `AacEncoder`: validates prepared signed 16-bit PCM input and AAC `PacketStore` track ownership, starts Media Foundation, enumerates AAC encoder MFTs, activates one when available, sets documented AAC-LC input/output media types, preserves the configured raw-AAC payload type and exact `AudioSpecificConfig` decoder bytes exposed through `MF_MT_USER_DATA`, and exposes in-memory AAC packet submission/drain/flush APIs.
- `AacPacketStore`: maps `EncodedAacPacket` to `PacketStore` AAC packet metadata and appends payloads to an existing packet store.
- `AacEncodeSink`: synchronously accepts prepared PCM blocks, submits them to an AAC encoder interface, drains packets, and appends through `AacPacketStore`.
- `PreparedPcmQueue`: owns a bounded in-memory queue for prepared PCM blocks destined for audio encoding, with explicit reject-newest and drop-oldest overflow policies.
- `AudioEncodeWorker`: synchronously drains prepared PCM blocks from `PreparedPcmQueue` into `AacEncodeSink` with an explicit per-call block budget and sink-failure stop behavior. It does not own a thread yet.
- `AudioTrackEncodeChain`: owns the synchronous per-track composition of captured PCM staging, bounded prepared PCM queue, AAC sink, and queue-drain worker with a track-specific captured input format. It does not own live capture or background thread lifetime yet.
- `AudioEncodeSession`: owns one `AudioTrackEncodeChain` per configured AAC track and routes captured PCM packets, their actual PCM formats, bounded or unbounded drains, and flushes by track id. It does not own live capture or background thread lifetime yet.
- `AudioEncodeSessionBinding`: validates audio track plans, per-track captured PCM format slots, queue options, and caller-provided encoder slots before building `AudioEncodeSessionTrack` entries in plan order.
- `AudioCaptureEncodeSetup`: preflights default direct WASAPI source formats, builds an AAC `AudioTrackPlan` plus per-track captured/prepared format metadata, rejects unsupported sample-rate conversion explicitly, initializes real or injected AAC encoders, requires complete encoder output metadata for future MP4 export, and assembles an `AudioEncodeSession` against a caller-owned `PacketStore`.
- `AudioSource`: owns captured source identity validation shared by source routing, source dispatching, and capture handoff.
- `AudioSourceRouter`: maps captured source identities to direct source AAC tracks from `AudioTrackPlan`, while rejecting disabled sources and the default mixed track because mixing is a separate future output.
- `AudioSourceSessionDispatcher`: resolves captured source identities and queues PCM plus its captured format into `AudioEncodeSession`, keeping source-route failures distinct from encode-session queue failures.
- `CapturedPcmSink`: defines the non-owning captured PCM packet view and sink dispatch boundary used by live capture handoff. PCM bytes are valid only during the sink call.
- `CapturedPcmSessionSink`: implements `ICapturedPcmSink` by forwarding validated captured PCM packets into `AudioSourceSessionDispatcher` with distinct invalid-config, route, and queue error statuses.
- `AudioCaptureEncodeBridge`: composes source routing, source-to-session dispatch, captured PCM sink handoff, and bounded `AudioEncodeSession` draining around a caller-owned encode session. It does not own live capture threads or recorder session lifetime.
- `AudioCaptureManager`: validates planned live audio sources, attaches direct source tracks to a caller-owned captured PCM sink, and marks process-loopback/default-mixed work as deferred. It does not start capture threads or own encoding/session state yet.
- `AudioCaptureSmoke`: runs `AudioCaptureManager` supported source bindings through duration-bounded default loopback/microphone smoke paths and a caller-owned sink. It supports injected source runners for automated tests and does not own live recording lifetime.
- `AudioCaptureEncodeSmoke`: composes duration-bounded source smoke with `AudioCaptureEncodeBridge`, drains the caller-owned encode session afterward, and reports capture versus drain failures separately. Automated tests use fake source runners and fake AAC encoders.
- `WasapiCaptureSource`: owns live start/stop lifetime for one supported default WASAPI source, currently system loopback or microphone, on an event-driven worker thread. It pushes captured packets into a caller-owned `ICapturedPcmSink`, applies the selected thread-scoped `Audio` MMCSS policy, and reports live stats plus the actual scheduling result without owning encoder, recorder-session, mux, or export lifetime.
- `AudioLiveCaptureEncode`: exposes explicit prepare/start/tick/stop/drain/flush ownership for supported live `AudioCaptureManager` sources against `AudioCaptureEncodeBridge`, serializes bridge sink/drain access across capture threads, and retains the duration-bounded wrapper for smoke paths. It relies on caller-owned `AudioEncodeSession` and `PacketStore`, allowing the production cross-media recorder to own finalization order.
- `AudioRecordingSession`: owns the recorder-facing audio-only preflight, AAC session setup, live capture/encode run options, and duration-bounded run orchestration around a caller-owned `PacketStore`. The caller still creates the store after preflight and owns cross-media recorder state.
- `AudioRecordingMetadata`: converts prepared direct-source track and AAC encoder state into validated per-track `SessionManifest` metadata, including stable source kind, display name, sample rate, channels, bitrate, frame size, raw payload type, profile/object type, exact decoder configuration, and encoder identity.
- `AudioTrackPlan`: maps enabled audio sources to future AAC `PacketStore` track definitions without starting capture, encoding, or persistence.
- `PcmAudio`: shared PCM stream format, packet timing, packet info, and capture stats types used by capture smoke paths and future encoders.
- `WasapiLoopbackCapture`: short default-render loopback smoke that counts PCM packets/frames and timestamps without encoding or persistence, with an optional caller-owned captured PCM sink for packet handoff plus a default render mix-format query for manual encode preflight. Idle render endpoints may report zero packets.
- `WasapiMicCapture`: short default-capture microphone smoke that counts PCM packets/frames and timestamps without encoding or persistence, with an optional caller-owned captured PCM sink for packet handoff plus a default capture mix-format query for manual encode preflight.
- `O'LouieAudioSmoke`: prints local endpoint discovery results and can run `--loopback [duration_ms]`, `--mic [duration_ms]`, manager-driven `--capture-sources [duration_ms]`, live `--live-sources [duration_ms]`, live real-AAC `--live-capture-encode [duration_ms]`, duration-bounded real-AAC `--capture-encode [duration_ms]`, or recorder-facing `--audio-session [duration_ms]`. The live-source path starts/stops supported default WASAPI sources against a caller-owned sink without encoding. The real-AAC paths create a temporary `PacketStore`, bind each direct source track to its own captured PCM format, initialize Media Foundation AAC encoders, report raw-AAC output metadata, run capture, flush, and report packet output; `--audio-session` does this through `AudioRecordingSession`.

Still deferred here:

- Process-tree application loopback.
- PCM sample-rate conversion/resampling.
- Default mixed track generation/mixing.
- A default mixed track and process-loopback sources in the production recorder.
