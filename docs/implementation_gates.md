# O'Louie Implementation Gates

This file keeps pass-to-pass progress orderly. `docs/progress.md` remains the running history; this file defines the gates that decide what work may start next.

## Pass Cadence

1. Re-read `clip_recorder_design.txt`, `clip_recorder_implementation_plan.txt`, and `docs/progress.md` before choosing a new implementation pass.
2. Pick the narrowest pass that advances the active gate.
3. Do not cross into a later milestone band until the current gate's evidence is present.
4. Update `docs/progress.md` with what changed, what was verified, and what remains intentionally deferred.
5. Run the most relevant build or verification command before reporting progress.

## Completed Gate: FFmpeg Video MP4 Writer

Milestones 3 and 5 reached the point where video-only H.264 packets can be stored, recovered, adapted into an MP4 mux request, and written when the explicit FFmpeg backend is configured. Pass 130 completed this gate against a locally built FFmpeg 8.1.2 LGPL-oriented dynamic development/runtime root.

Completion evidence:

- The pinned official `ffmpeg-8.1.2.tar.xz` source archive passed GPG verification with FFmpeg release-key fingerprint `FCF986EA15E6E293A5644F10B4322F04D67658D8`; the source-build helper records local provenance in the generated, ignored `docs\ffmpeg_source_build_provenance.md` file.
- The local shared MSVC root at `_deps\ffmpeg` passed `tools\VerifyFfmpegRoot.ps1` with headers, import libraries, and runtime DLLs for `avformat`, `avcodec`, `avutil`, and `swresample`.
- The required verifier run published `build-ffmpeg\ffmpeg-record-test-output\verification-report.txt` and its 852-byte `O'LouieRecordTests\exports\h264.mp4`; the strict gate checker reported `Verified`.
- The optional live WGC run also passed on an NVIDIA GeForce RTX 3060 through the NVIDIA H.264 Encoder MFT, encoded three copied frames, wrote a 173,190-byte `wgc-smoke.mp4`, and passed the repository MP4 structure/payload inspector. This was structural/runtime verification, not player playback verification.
- The default no-FFmpeg Debug build continued to pass all five registered tests and retained the explicit backend-unavailable behavior.

- `.\tools\CheckFfmpegMuxerGate.ps1` reports the gate is ready or verified.
- `.\tools\VerifyFfmpegRoot.ps1 -FfmpegRoot <path>` succeeds for the selected local root, or succeeds with explicit include/lib/bin directories.
- `.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot <path>` succeeds.
- FFmpeg-enabled `O'LouieRecordTests` run against the configured build and produce the video-only MP4 writer test path.
- The direct verifier-run record test leaves `O'LouieRecordTests\exports\h264.mp4` under `build-ffmpeg\ffmpeg-record-test-output` or a caller-provided directory-valued `-RecordTestOutputDir`.
- `tools\InspectMp4Artifact.ps1` succeeds against that preserved `h264.mp4`, confirming a nonempty MP4 top-level shape with an `ftyp` box plus nonempty `mdat` media-data and `moov` movie-metadata payloads.
- `tools\VerifyFfmpegMuxer.ps1` atomically publishes a schema-versioned timestamped success-only `verification-report.txt` beside the preserved output by default, or to a caller-provided `-VerificationReportPath`, by writing through a same-directory temporary file before replacing the final report path. The report records verifier provenance, FFmpeg archive input, WGC smoke status/options, plus the preserved MP4 artifact path, size in bytes, and last-write UTC.
- Default no-FFmpeg builds continue to pass and continue to report the backend as unavailable instead of falling back.

Optional evidence before moving into broad recorder ownership:

- `.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot <path> -RunWgcSmoke` succeeds after screen-capture consent is acceptable.
- The produced smoke MP4 is inspected in a player or with an external tool.

Development policy retained after completion:

- Codex owns local development setup and has standing authority to download, install, update, configure, and use any tools or dependencies needed to develop and verify O'Louie without handing routine setup to the user. This includes FFmpeg, GPG, MSYS2, `make`, `nasm`, `cmp`/diffutils, SDKs, package managers, media inspectors, and other build/test prerequisites. Gate text must describe missing routine dependencies as Codex-owned pending work, never as something the user must provide.
- FFmpeg automation must remain licensing-conscious, but it is not limited to files already present on the machine. Codex may acquire or build a suitable local FFmpeg development root from the repository's preferred pinned official-source path or another vetted source or binary distributor. Use an LGPL-oriented dynamic FFmpeg root, record provider/version/source/hash/license/linkage provenance when material, and do not enable GPL/nonfree components unless the project licensing direction is intentionally changed. Development setup authorization does not grant permission to bundle, redistribute, or statically link FFmpeg in shipped app packages.
- `tools\PrepareFfmpegRootFromArchive.ps1` may prepare one combined ZIP archive or multiple ZIP archives that together provide `include`, `lib`, and `bin`. The helper does not acquire archives itself, but Codex may download suitable inputs before invoking it.
- `tools\PrepareFfmpegRootFromArchive.ps1 -DestinationDir <dir>` must name a directory inside the repository, not an existing file, and its parent chain must not be blocked by an existing file. `-Force` may replace an existing destination directory, but it must not remove a file-valued destination.
- `tools\PrepareFfmpegRootFromArchive.ps1 -InspectOnly` may preflight a local archive set without preparing `_deps\ffmpeg`; inspection alone is not muxer verification evidence.
- `tools\CheckFfmpegMuxerGate.ps1 -FfmpegArchivePath <archive.zip>[,<archive.zip>]` may inspect archive candidates from the gate status command, but a successful archive inspection still reports pending until a local root is prepared and the muxer verifier runs. Archive inspection is exclusive with prepared-root status input and gate-run verification; do not combine `-FfmpegArchivePath` with `-FfmpegRoot`, split include/lib/bin directories, or `-RunVerification` in the same gate-status check.
- `tools\VerifyFfmpegMuxer.ps1 -FfmpegArchivePath <archive.zip>[,<archive.zip>]` may prepare `_deps\ffmpeg` from a local archive set and then run the normal configured muxer verifier. The archive-preparation helper itself does not download, install, license-vet, or bundle FFmpeg, but Codex may automatically acquire and vet suitable archive inputs before invoking it. Archive preparation input is exclusive with prepared-root input here too; do not combine `-FfmpegArchivePath` with `-FfmpegRoot` or split include/lib/bin directories in one verifier command.
- `tools\BuildFfmpegLgplFromSource.ps1` may automatically download only the pinned official FFmpeg 8.1.2 source archive and `.asc` signature from `ffmpeg.org`, require local GPG signature verification, build a local Windows x64 shared-library root with MSVC/MSYS2 using LGPL-oriented flags that disable GPL, nonfree, and version3 components, verify the resulting root, and write `docs\ffmpeg_source_build_provenance.md`. It still does not bundle or redistribute FFmpeg, and the generated `_deps\ffmpeg` root is development-only. Local tools may be supplied with explicit `-TarPath`, `-GpgPath`, `-BashPath`, and `-VcVarsAllPath` arguments when they are installed outside `PATH`. If `-DestinationDir` or `-WorkDir` are overridden, they must be separate child directories under `_deps`; the helper must not generate, replace, or delete source-build directories elsewhere in the repository.
- `tools\BuildFfmpegLgplFromSource.ps1 -DescribePlan` is allowed to print the source-build plan without downloading, extracting, building, or writing files. `tools\BuildFfmpegLgplFromSource.ps1 -CheckPrerequisites` is allowed to check local tools and report missing prerequisites without downloading, extracting, building, or writing files; it exits nonzero when required tools are missing. A successful source build only makes the local root ready for `tools\VerifyFfmpegMuxer.ps1`; it is not muxer verification evidence by itself.
- `tools\VerifyFfmpegMuxer.ps1` treats `-FfmpegArchiveDestinationDir` and `-ForceFfmpegArchivePreparation` as archive-preparation-only options. They require `-FfmpegArchivePath` and are rejected before any existing verification report is cleared when no archive input is supplied.
- `tools\VerifyFfmpegMuxer.ps1 -FfmpegArchiveDestinationDir <dir>` applies the same inside-repository directory, file-target, and parent-chain validation before clearing any existing verification report or invoking archive preparation.
- Explicit split-directory FFmpeg input is valid only when all three of `-FfmpegIncludeDir`, `-FfmpegLibraryDir`, and `-FfmpegBinaryDir` are supplied, unless `-FfmpegRoot` is also supplied as the base root.
- `tools\VerifyFfmpegMuxer.ps1 -RecordTestOutputDir <dir>` may redirect the preserved direct `O'LouieRecordTests` output for gate evidence, but the path must not be an existing file and its parent chain must not be blocked by an existing file. Normal CTest/default test runs still clean up their temporary record-test folders. The verifier requires the FFmpeg-backed `O'LouieRecordTests\exports\h264.mp4` artifact after the direct test run.
- `tools\VerifyFfmpegMuxer.ps1 -VerificationReportPath <file>` may redirect the success report, but the path must name a report file, not an existing directory, its parent chain must not be blocked by an existing file, and it must not resolve to the same path as `-RecordTestOutputDir`. After static verifier option and output-target validation, the verifier removes any existing file at the resolved report path before archive preparation, root verification, build, or test work begins; a new report is written to a same-directory temporary file and moved into place only after all requested verifier steps succeed.
- `tools\CheckFfmpegMuxerGate.ps1` applies the same evidence-target path validation for prepared-root or complete split-directory checks before reading report evidence or forwarding `-RunVerification` to the verifier.
- When `tools\CheckFfmpegMuxerGate.ps1` reports a prepared root or complete split-directory input as ready, its suggested `tools\VerifyFfmpegMuxer.ps1` command preserves caller-supplied `-RecordTestOutputDir` and `-VerificationReportPath` values so the next verifier run writes evidence to the same locations the gate checker inspected.
- `tools\VerifyFfmpegMuxer.ps1` rejects nonpositive `-WgcDurationMs`, `-Width`, `-Height`, `-Fps`, and `-BitrateMbps` values before preparing archives, building, running tests, or writing a success report, so the verifier cannot create gate-invalid WGC smoke option evidence.
- `tools\CheckFfmpegMuxerGate.ps1 -RunVerification` forwards `-RunWgcSmoke`, `-WgcDurationMs`, `-Width`, `-Height`, `-Fps`, and `-BitrateMbps` to `tools\VerifyFfmpegMuxer.ps1` for prepared roots or explicit split include/lib/bin directories. If no prepared root or complete split-directory input is available, `-RunVerification` exits nonzero instead of reporting a passive Pending status. WGC smoke options on the gate checker require `-RunVerification` so status-only checks do not silently ignore them.
- `tools\CheckFfmpegMuxerGate.ps1 -RequireVerified` may require a current ready root plus a consistent success report and preserved MP4 artifact before returning success; the success report must contain no duplicate keys, malformed non-empty lines, or unexpected schema-version-1 keys, use the current report schema, identify `tools\VerifyFfmpegMuxer.ps1` as verifier, name the same build configuration currently being checked, name the same FFmpeg root or split include/lib/bin directories currently being checked, include the `ffmpeg_archive_input` field, include WGC smoke status of `skipped` or `passed`, include WGC smoke options with positive integer `duration_ms`, `width`, `height`, `fps`, and `bitrate_mbps` values, have a generation timestamp that is not older than the current preserved MP4 artifact and not more than 5 minutes in the future, and its MP4 artifact size and last-write UTC must match the current preserved file. For root-only or split-directory verifier runs, `ffmpeg_archive_input: <not supplied>` is valid; archive-preparation verifier runs record the archive path set. A missing or empty archive-input field is invalid. This is a gate-status check, not a substitute for running the real verifier once.
- `tools\InspectMp4Artifact.ps1` is a repository-owned MP4 structure and payload sanity check for gate evidence; it is not a playback, codec, duration, sync, or ffprobe replacement.

Status command:

```powershell
.\tools\CheckFfmpegMuxerGate.ps1
```

Tooling self-test:

```powershell
.\tools\TestFfmpegGateTools.ps1
```

The self-test uses synthetic archive contents, synthetic MP4 box fixtures, and synthetic verification reports to validate the repository helper scripts, expected folder layout, archive inspection, archive preparation destination path validation, archive candidate gate reporting, verifier archive-input failure routing, verifier archive-preparation option requirement validation, verifier WGC/report option validation, muxer verifier stale-report cleanup, muxer verifier archive-destination path validation, muxer verifier report-directory target rejection, verifier record-output file target rejection, verifier report/record-output same-path rejection, verifier evidence-target parent/ancestor-file rejection, gate checker evidence-target path validation, gate checker WGC run-verification option validation, gate run-verification input requirement reporting, FFmpeg source build dry-plan/prerequisite reporting/explicit tool path reporting/generated path guards, MP4 artifact box/payload inspection, verification-report evidence recognition, verification-report schema/provenance/configuration/timestamp/future-timestamp/archive/duplicate-key/malformed-line/unexpected-key/WGC-smoke option-schema detail checks, root/split FFmpeg-input consistency checks, partial split-directory rejection, gate-status and muxer-verifier archive/root input conflict rejection, gate archive run-verification conflict reporting, archive-input field presence/empty/root-only semantics, artifact metadata consistency checks, gate-ready/verified reporting, root and split-directory CMake FFmpeg option wiring, and explicit failure behavior for incomplete roots. It does not replace real FFmpeg verification because it does not use real FFmpeg binaries, link against FFmpeg, download or verify source, build FFmpeg from source, or run FFmpeg-enabled record/mux tests.
It also checks that ready-state verifier command guidance carries caller-supplied evidence targets through for prepared-root and split-directory checks.
When `OLOUIE_BUILD_TESTS=ON` and PowerShell is available, this self-test is also registered as `OLouieFfmpegGateToolTests` in CTest.

## Completed Gate: First User-Usable Video-Only Recording Flow

Pass 131 completed the smallest real tray-driven recording flow by composing WGC, hardware H.264, `PacketStore`/session metadata, recovery/export planning, and the verified FFmpeg MP4 writer.

Completion evidence:

- `VideoRecorderSession` runs the ordered recorder pipeline off the hidden-window/message thread and exposes `Idle`, `Starting`, `Recording`, `Stopping`, `Saved`, and `Failed` states.
- `Ctrl+Shift+R` and the dynamic tray command use the same start/stop path. The tray reports preparing, recording, stopping/saving, the saved output path, and failures.
- `WgcMonitorCaptureSession` continues until an explicit stop, copies frames into bounded app-owned resources, blocks new delivery during shutdown, and exposes capture stats/errors.
- The stop path executes capture stop, queued-frame drain, final hardware H.264 drain, manifest write, `PacketStore` close/recovery, export-plan creation, and atomic FFmpeg MP4 write in that order.
- A final configured-app run on 2026-07-18 recorded the primary 1920x1080 monitor through the NVIDIA H.264 Encoder MFT, accepted start and stop through the registered hotkey, saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260718-210036-039-p15780-0.mp4`, remained responsive after save, and exited cleanly through `WM_CLOSE`.
- The saved file is 791,084 bytes. `tools\InspectMp4Artifact.ps1` found `ftyp` (32 bytes), nonempty `mdat` (787,823 bytes), and `moov` (3,221 bytes). Independent headless VLC parsing reported 4,675 ms duration, started the H.264 `avcodec` decoder, reached EOF, and exited at the end of the playlist.
- Focused tests cover finalization order, partial-start cleanup, asynchronous state transitions, repeated/invalid commands, cancellation during start, and backend-unavailable-before-capture behavior. Unexpected worker exceptions are contained as explicit failures by the session boundary.
- The final default Debug build passed all five registered tests. The final FFmpeg verifier also passed all five registered tests, reran `O'LouieRecordTests` directly, inspected and republished its preserved MP4/report evidence, and the strict gate checker reported `Verified`; no-FFmpeg behavior remains explicit and does not select another muxer.

## Completed Gate: Full Recording With System Audio And Optional Mic

Pass 133 extended the proven tray recorder into a trustworthy full recording with synchronized direct audio while reusing the existing WASAPI/AAC, `PacketStore`, recovery, export-plan, and FFmpeg command flow.

Required evidence:

- The normal recorder honors the existing audio settings: system loopback is enabled by default and microphone capture is optional.
- Start preflights requested WASAPI endpoints/formats and hardware video plus AAC encoders before entering `Recording`, with explicit source/format/encoder failures and cleanup of partial starts.
- One long-running recorder lifetime owns WGC video plus requested live WASAPI sources, routes H.264 and direct-source AAC packets into distinct tracks in the same session, and derives timestamps from a compatible monotonic session origin.
- Stop first prevents new video/audio input, then drains video frames/H.264 output and prepared PCM/AAC output, flushes encoders, writes complete cross-media metadata, closes and recovers `PacketStore`, and builds one interleaved export plan.
- `SessionManifest`, recovery/export planning, and the dynamic FFmpeg muxer carry required AAC metadata and write one MP4 containing the H.264 video track plus each enabled direct audio track that emitted packets. Default mixed-track generation may remain deferred for this gate.
- The existing tray/hotkey path remains responsive and reports audio-aware start, save, and failure results without adding a continuous hidden UI render loop.
- Focused tests cover audio-disabled, system-only, system-plus-mic, endpoint/format failure, partial-start cleanup, stop/drain/flush ordering, timestamp normalization, AAC metadata, and mixed H.264/AAC mux planning/writing.
- A real configured-app run captures actual system-loopback audio with video, saves a nonempty MP4, passes repository structural checks, and is independently inspected for H.264 video plus playable AAC audio and plausible duration/synchronization. A microphone-enabled run is also verified when an input endpoint is available.
- Default no-FFmpeg builds continue to compile and test, and recording fails explicitly before capture when final MP4 writing is unavailable.

Completion evidence:

- Media Foundation AAC setup now requires and preserves the exact raw-AAC `AudioSpecificConfig` bytes exposed by the configured encoder output type, alongside sample rate, channels, bitrate, frame size, payload/profile/object type, and encoder identity.
- Session manifest version 2 persists zero or more direct AAC tracks and rejects duplicate/incomplete track metadata; legacy version 1 video-only sessions remain readable.
- Recovery/export planning keeps every current-manifest H.264/AAC packet-bearing track in one normalized interleaved plan and rejects unmanifested or codec-mismatched packets. Pass 139 later established the explicit empty-track policy for configured sources that emit no packets.
- The dynamic FFmpeg writer configures one H.264 stream plus each AAC stream, applies AVCC and AAC decoder configuration, checks DTS per track, passes raw AAC access units through, and writes one atomically finalized MP4.
- Focused fake-backed tests cover AAC metadata preservation/mismatch, manifest round trips, mixed export planning, mixed request validation, stream setup, payload accounting, and H.264/AAC packet writing. A preserved 1,456-byte synthetic artifact at `build-ffmpeg\pass132-final-record-output\O'LouieRecordTests\exports\h264-aac.mp4` passed the repository MP4 inspector; VLC found two tracks and classified track 1 as video and track 2 as audio.
- A real 500 ms `O'LouieAudioSmoke --audio-session` run initialized system and microphone tracks through the Microsoft AAC Audio Encoder MFT, reported 48 kHz stereo raw AAC at 192 kbps with 1024 samples per frame and two-byte decoder configuration, captured 104 PCM packets, flushed both tracks, and wrote 49 AAC packets to its temporary `PacketStore`.
- `SessionClock` captures one QPC-derived origin in the WGC/WASAPI 100 ns clock domain after encoder setup. Production video and audio normalization use that same zero; tests verify equal video/audio offsets and reject competing video origins.
- `AudioLiveCaptureEncodeSession` exposes explicit prepare/start/tick/stop/drain/flush ownership. `VideoRecorderPipeline` preflights requested audio, combines H.264/AAC definitions in one store, starts direct audio and WGC, stops both input families, drains queued video and PCM, finalizes H.264, flushes AAC, attaches audio metadata, and runs the existing recovery/export/mux path.
- Focused tests cover source/format failures, system-only and system-plus-mic planning, partial audio starts, source-stop before PCM drain before AAC flush, shared-origin normalization, runtime-failure cleanup, AAC metadata, and mixed MP4 planning/writing. Both default and configured Debug suites pass all five registered tests; the final strict FFmpeg verifier passed and the gate checker reported `Verified`; no-FFmpeg behavior remains explicit before capture.
- A real configured tray/hotkey run with default system loopback saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260718-225622-332-p36400-0.mp4` at 7,420,321 bytes. The repository inspector found `ftyp`, nonempty `mdat`, and `moov`; the app remained responsive after save and exited cleanly through the hidden message window.
- Headless VLC exited 0, found H.264 video plus AAC audio, instantiated `avcodec` and `faad`, decoded AAC to 48 kHz stereo float output with zero decoder errors, and reached EOF. Video/audio edit-list starts differed by 9 ms and reported durations differed by 18 ms (4,697 ms versus 4,715 ms), providing plausible real A/V synchronization evidence.
- No active/default capture endpoint was available during final Pass 133 verification (`0x80070490`), so the conditional microphone-enabled runtime check could not run. Optional mic preflight, encoder setup, live ownership, metadata, and partial-failure behavior remain fake-backed and the earlier real AAC smoke evidence is retained.

Development policy retained after completion:

- Keep process-loopback capture, default mixed-track generation, and PCM sample-rate conversion deferred until a later audio/settings need justifies them.
- Keep broader settings UI and unrelated polish deferred while the clip/bookmark gate is active.

## Completed Gate: Active-Recording Clips And Bookmarks

The next milestone is to save keyframe-aligned MP4 clips from the live H.264/AAC `PacketStore` while capture continues, then complete the remaining preset/bookmark command flow. Reuse the existing recorder command flow and mixed export/mux boundaries; do not create a second recording session or block capture callbacks on export.

Required evidence:

- The recorder can take a thread-safe active-session packet/index snapshot or query after flushing writer visibility, without stopping WGC/WASAPI or closing the `PacketStore`.
- A "save last duration" request clamps to available session time, aligns to the previous H.264 keyframe, includes overlapping enabled AAC tracks, and builds the existing normalized mixed mux request.
- MP4 mux work runs away from capture callbacks and does not stall the hidden Win32 message thread. Queue ownership, repeated commands, shutdown, and export failure are explicit.
- The existing tray/hotkey command reports queued/saved/failed clip state while the primary recording remains active. Add one duration first before expanding presets.
- Focused tests cover short-session clamping, keyframe alignment, AAC overlap, concurrent/repeated export requests, export failure, shutdown, and continued packet capture during export.
- A real configured run saves a playable H.264/AAC clip while the full recording continues, then stops and saves the full recording successfully.

Pass 135 first-clip evidence:

- `PacketStore::SnapshotForExport` flushes `packets.dat` and copies paths/index under the writer lock. `ActiveRecordingClip` computes the available video end from that immutable snapshot, clamps short sessions, aligns to the previous H.264 keyframe, includes overlapping AAC, and builds the existing mixed `VideoExportPlan`/`Mp4MuxRequest`.
- `VideoRecorderClipCommandQueue` bounds Win32-to-recorder requests. `ClipExportQueue` bounds outstanding mux jobs, runs one below-normal-priority worker, reports success/failure, rejects excess/repeated work explicitly, and drains before session finalization. Clip failure events leave recorder state unchanged.
- `Ctrl+Shift+F8` and the recording-only tray command **Save Last 30 Seconds** report queued/saved/failed through the hidden-window notification marshal while the recorder worker continues WGC/WASAPI drain ticks.
- Focused tests cover short-session clamping, previous-keyframe alignment, overlapping AAC, live payload visibility, bounded repeated requests, background-thread execution, injected mux failure, drain-on-shutdown, valid post-shutdown rejection, packet appends during a blocked export, idle clip rejection, and saved/failed clip events that preserve `Recording`.
- A real configured run saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260718-234139-011-p2684-0-clip-1.mp4` (2,745,590 bytes) while the app remained active, continued recording for about six seconds, then saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260718-234139-011-p2684-0.mp4` (3,910,616 bytes) and exited cleanly. Both passed `InspectMp4Artifact.ps1` with `ftyp`, nonempty `mdat`, and `moov`.
- Headless VLC exited 0 for both files, found two tracks, started H.264 `avcodec` plus AAC `faad`, decoded audio to 48 kHz stereo float, reached EOF, and reported no decoder/demux corruption errors. The clamped clip duration was about 5.5 seconds and the full recording about 12.4 seconds, proving capture continued after clip save. The single initial zero-sample AAC priming warning appeared in each file.
- Default and configured Debug suites passed all five registered tests. The final strict FFmpeg verifier republished its preserved artifact/report, and `CheckFfmpegMuxerGate.ps1 -RequireVerified` reported `Verified`.

Pass 136 completion evidence:

- Typed requests and completions distinguish duration clips from bookmark exports while preserving one bounded command queue and one low-priority mux queue. `Ctrl+Shift+F9` plus **Save Last 5 Minutes** use the same duration planner as the 30-second command; `Ctrl+Shift+F10` plus **Add Bookmark and Save Clip** use the same queue lifecycle and are disabled outside `Recording`.
- `ActiveRecordingBookmark` takes the current encoded H.264 end returned by the flushed active clip snapshot as marker time, adds the marker to the recorder worker's session-owned `BookmarkCollection`, resolves the default 60-second pre-roll/zero-post-roll window, and keeps previous-keyframe/AAC planning in the existing active clip path. Export failure remains nonfatal and does not discard an already-created marker.
- Session manifest version 3 atomically writes validated bookmark ids, marker times, labels, default pre/post windows, and notes beside H.264/AAC metadata. Version 1 video-only and version 2 H.264/AAC manifests remain readable; current manifests reject duplicate/invalid bookmark ids and data.
- Focused tests cover manifest v3 bookmark round trips and v2 compatibility, live marker timing, short-session clamping, previous-keyframe/AAC inclusion, explicit nonzero-post-roll deferral, typed mixed FIFO requests, bounded/repeated work, shutdown rejection, bookmark metadata through background completion, idle rejection, 5-minute requests, and saved/failed bookmark events that preserve `Recording`.
- A final configured tray/hotkey run saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260719-002118-607-p11776-0-clip-1.mp4` (1,035,083 bytes), then created bookmark 1 and saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260719-002118-607-p11776-0-bookmark-1.mp4` (1,046,602 bytes), continued recording, and saved `%LOCALAPPDATA%\O'Louie\exports\O'Louie-20260719-002118-607-p11776-0.mp4` (1,518,352 bytes). The process remained alive after each finalized export and full save, then exited 0 through `WM_CLOSE`.
- That run's version 3 `session.json` persisted bookmark 1 at 5,571,782,700 ns with 60,000,000,000 ns pre-roll and zero post-roll. All three MP4s passed `InspectMp4Artifact.ps1`. VLC exited 0 for each, found H.264 plus AAC, started `avcodec` and `faad`, decoded 48 kHz stereo audio, reached EOF/end-of-playlist, and had zero targeted decoder/demux corruption errors. Durations were about 5.17, 5.48, and 9.23 seconds, proving clamping and continued capture after both active exports.
- The final default Debug build and final strict FFmpeg verifier each passed all five registered tests. The verifier reran configured `O'LouieRecordTests`, inspected and republished its preserved 852-byte artifact/report, and the final strict gate checker reported `Verified`.

Retained limitation:

- Immediate active bookmark export currently supports configurable pre-roll and zero post-roll. Nonzero post-roll needs deferred job completion after future packets arrive and remains future work; it is rejected explicitly rather than producing a shortened or false export.
- MP4 muxing, disk-heavy export work, and player inspection remain prohibited on capture callback and hidden-window threads.

## Completed Gate: Persisted Dear ImGui Settings UI

Pass 138 completed a real event-driven settings surface for the already-working recorder, clip presets, bookmark behavior, audio sources, video settings, output location, and hotkeys through the established Dear ImGui + Win32 + D3D11 direction.

Required evidence:

- Settings are loaded from and atomically saved to a versioned file under the existing runtime settings directory. Missing files use defaults; malformed/unsupported data produces an explicit diagnostic and safe behavior; validation and any migrations are testable.
- The tray/double-click **Open Settings** action opens the actual settings window. The window exposes the current General, Video, Audio, Clips/Bookmarks, and Hotkeys data needed for the MVP rather than a placeholder or feature-description page.
- Dear ImGui is acquired from an official, pinned, documented source and integrated through the Win32/D3D11 backends without changing FFmpeg licensing/shipping behavior.
- The settings window and any user-visible notification window apply `WDA_EXCLUDEFROMCAPTURE` where supported and report exclusion failure in diagnostics without blocking use.
- When settings are closed, no ImGui frame/render loop or settings swap-chain churn runs. When visible, rendering is message/timer driven and capped; the hidden recorder/tray message path stays responsive.
- Valid changes persist and affect the next recording/export command. Hotkey changes re-register explicitly and report conflicts without silently losing the previous working binding.
- Focused tests cover persistence round trips, malformed/unsupported files, validation, hotkey mapping/conflicts through injectable boundaries, visibility/lifetime state, and zero hidden rendering. A real configured run opens/changes/reopens settings, records and exports with an applied setting, and preserves the completed full-recording/clip/bookmark behavior.

Pass 151 live microphone-check evidence:

- Settings schema 2 persists a stable mic-check render-endpoint ID while loading schema-1 files with Windows Default selected. Missing saved outputs retain their ID and resolve to the current default for monitoring.
- `MicMonitorSession` owns nonblocking start/stop state and a test-injectable backend. Its production backend performs event-driven shared-mode default-microphone capture and selected/default output rendering on a worker, uses Windows audio-engine conversion, bounds PCM buffering to roughly 250 ms, reports dBFS/clipping and queue pressure, and retries an invalidated output through Windows Default.
- `AppHost` owns the session, rejects monitor starts during recording, and defers a requested recording start until an active monitor reports stopped. Settings close/cancel/reset and app shutdown stop monitoring without putting WASAPI work on the message thread.
- Focused settings/audio tests cover schema migration and round-trip, asynchronous lifecycle/repeated commands/backend failure, selected/fallback output snapshots, and float/signed-16 peak measurement. The Audio-tab smoke path renders representative monitoring/fallback/meter state and retains the existing visible-only/capture-exclusion lifecycle checks.

Pass 154 capture-first scheduling evidence:

- The recorder/audio-service worker, WGC callback scope, live WASAPI source
  threads, and dedicated video encode worker now register with Windows MMCSS.
  Video work uses the `Capture` task and audio sources use `Audio`. Balanced
  uses normal relative priority and Capture First uses high; every registration
  is best-effort, thread-scoped, reverted on exit, and forbidden from changing
  process priority.
- Settings schema version 3 persists Balanced/Capture First and migrates both
  version-1 and version-2 files to Balanced in memory. The Video UI explains
  the encoder-quality/game-FPS tradeoff, while unsupported newer settings remain
  protected from overwrite.
- Capture First requests `CODECAPI_AVLowLatencyMode=1` and
  `CODECAPI_AVEncCommonQualityVsSpeed=0`. Diagnostics retain support,
  modifiability, SetValue result, readback availability, accepted value, and
  driver message for every codec control.
- Default and FFmpeg-configured Debug builds/tests each passed 6/6. A real
  NVIDIA GeForce RTX 3060 / NVIDIA H.264 Encoder MFT BGRA smoke accepted and
  read back both Capture First values and recovered three of three packets. No
  full game-load recording or microphone was required for this phase.

Hold line:

- Start with versioned settings persistence plus the smallest usable event-driven settings host, then fill the current MVP tabs through the same ownership boundary. Do not substitute a static Win32 form or placeholder for the documented Dear ImGui direction.
- Keep process-loopback audio, default mixed-track generation, PCM resampling, DXGI fallback, broader robustness/recovery work, packaging, and unrelated polish deferred until the settings gate is complete.

Completion evidence:

- Official Dear ImGui `v1.92.8` is pinned under ignored `_deps\imgui`, acquired/validated by `tools\PrepareDearImGui.ps1`, compiled with the official Win32/DX11 backends, and documented with upstream URL, SHA-256, MIT license, and development-only provenance in `docs\dear_imgui_provenance.md`. The root build and direct CMake configuration reject missing or wrong-version trees.
- `SettingsStore` loads and atomically replaces schema version 1 under `%LOCALAPPDATA%\O'Louie\settings`; missing, malformed, invalid, unsupported-version, oversized, and I/O cases return explicit outcomes and safe defaults. Focused tests cover missing defaults, full round trip, clean atomic publication, malformed/invalid fallback, unsupported-file preservation, and invalid-save nonreplacement.
- `SettingsUi` exposes General, Video, Audio, Clips/Bookmarks, and Hotkeys tabs over one validated draft. `AppHost` loads settings before hotkeys/recorder creation, applies hotkey changes transactionally with previous-binding restoration on conflicts, synchronizes start-with-Windows registration, atomically persists the candidate, rolls back runtime changes on failure, and applies clip/notification changes immediately while deferring recorder rebuild until an active session ends.
- `ImGuiDx11Host` applies `WDA_EXCLUDEFROMCAPTURE`, reports failure without blocking use, renders from a visible-only 33 ms timer, performs no ImGui work from `WM_PAINT`, and kills the timer when hidden. The final visual/lifecycle smoke rendered 11 visible frames, kept the count at 11 after hiding, confirmed capture exclusion, and produced `build\pass138-settings-ui-final.bmp`; focused state and real-host tests protect hidden rendering and destruction.
- Both the final default and FFmpeg-configured Debug suites passed all five registered tests. The configured verifier regenerated and inspected its 852-byte H.264 MP4 artifact/report, and the strict FFmpeg checker reported `Verified`.
- A real configured app run opened the production settings window through the tray command, changed the output directory, saved, closed, and reopened it, then recorded into the applied folder. After supplying real system-loopback audio, the same running session saved a 10,219,341-byte live clip and 10,014,888-byte bookmark clip, continued capture, and saved a 21,695,376-byte full recording. All three passed the repository MP4 inspector; VLC initialized H.264 `avcodec` and AAC `faad`, reached end of playlist, and exited 0 for each. The app exited cleanly through its tray command and the pre-run missing-settings state was restored.
- An initial live clip request made before any system-loopback packets existed failed explicitly with `PacketStore does not contain a manifest audio track` and did not stop recording. This became the first robustness task and was resolved by Pass 139 without changing the completed settings behavior.

## Active Gate: Robustness And Diagnostics

This milestone makes the proven recorder/settings flow resilient to ordinary failure and long-running conditions. Pass 139 completed enabled-but-silent audio handling, Pass 140 completed selected-monitor resize/disconnect handling, Pass 141 completed D3D11/hardware-encoder device-loss handling, Pass 142 completed disk-space/write-failure handling, Pass 143 completed startup recovery, Pass 144 completed broader diagnostics, Pass 145 completed the bounded runner plus shorter export-under-load and interrupted-finalization recovery exercises, Pass 146 completed the runner's graceful external-stop evidence path, Pass 147 made final duration evidence independently machine-checkable, Pass 148 added strict codec/audio-configuration proof, Pass 149 made current-file artifact reinspection mandatory, and Pass 150 made resource-growth facts independently reproducible. The explicit longer-than-one-hour runtime criterion remains open without changing the hardware-only video or LGPL dynamic FFmpeg direction.

Required evidence:

- A configured audio source that produces no packets for part or all of a session no longer prevents live clip, bookmark, or full MP4 export. The chosen silence/empty-track policy is explicit, preserves A/V timeline semantics, and is covered by mixed-plan/writer tests plus a real silent-desktop run.
- Monitor loss/resize and D3D11/encoder device loss stop or recover through explicit state transitions, preserve recoverable session data where possible, and never silently select CPU video encoding.
- Disk-space/write failures stop unsafe work, keep capture/export queues bounded, report the failed subsystem, and preserve recoverable metadata/store data where possible.
- Startup scans for recoverable sessions and diagnostics expose useful recorder facts and failures, including selected monitor/encoder, capture/encode/drop counts, audio tracks, bitrate/FPS, and export errors, without creating hidden high-frequency work.
- Focused fault-injection tests cover cleanup order, repeated failures, recovery scanning, and state/notification behavior. A real configured long-session/export-under-load run and at least one practical recovery/failure exercise complete without corrupt output or an unresponsive tray path.

Pass 140 selected-monitor topology evidence:

- WGC content-size checks, `GraphicsCaptureItem::Closed`, and selected-monitor handle/device/rectangle polling feed one typed first-fault latch. Resize/disconnect are distinct from invalid-frame, copy, sink, and callback failures; repeated signals do not replace the first report.
- Resize/disconnect request ordered preservation finalization: stop inputs, drain queues/encoders/clip work, write the manifest, close/recover the packet store, and write MP4. The recorder still reports an explicit stopped/failed state, carries `recording_saved_after_failure`, names the changed monitor dimensions or disconnect, and includes the saved path. No monitor or encoder fallback was added.
- Focused tests cover resize/disconnect classification, repeat signals, ordinary abort versus preservation, one failure tick, complete finalization order, retained output, and user-facing session state.
- A real configured run changed `\\.\DISPLAY1` from 1920x1080 to 1680x1050 through the restore-armed `O'LouieDisplayModeExercise`. O'Louie stopped automatically, saved a 1,954,212-byte MP4, retained a 1,978,559-byte packet store and 2,104-byte manifest, stayed responsive, and exited cleanly after the display returned to 1920x1080. The MP4 inspector passed; VLC found one H.264 track with 662 samples/about 12.7 seconds, decoded through D3D11VA, reached EOF, and exited 0.
- Both final Debug suites passed all five tests. The configured verifier republished its required 852-byte artifact/report and the strict checker reported `Verified`.

Pass 142 disk-space/write-failure evidence:

- One shared `DiskWriteFault` carries cause, subsystem, operation, path, standard error code, FFmpeg backend code, and a user-facing description. It covers PacketStore create/append/flush/close, session-manifest temp writing and publication, and clip/full MP4 stale-temp/open/header/packet/trailer/close/publication boundaries. C++ stream sites capture `errno`; FFmpeg `AVERROR` and Win32 system errors retain their native codes.
- A PacketStore failure during video, audio, active-snapshot, queue-drain, or encoder-drain work requests ordered preservation finalization. Capture stops once, queue sizes remain bounded, queued video is discarded, further writes to the failed store are skipped, and clip work is shut down before manifest/store recovery and MP4 export are attempted. A later cleanup/finalization error cannot replace the first typed disk diagnosis. Background clip mux failures remain job-local and publish the typed path through the existing failure event.
- Manifest and MP4 final publication use same-directory `MoveFileExW` replacement, so overwrite no longer deletes the old destination before rename. Failed content/header/packet/trailer writes clean incomplete temps; failed atomic publication retains a complete temp and leaves the prior destination untouched. PacketStore recovery preserves its complete packet prefix, and a previously published manifest remains unchanged after a failed replacement.
- Focused tests cover simulated `no_space_on_device`, subsystem/operation/path text, hostile PacketStore and manifest paths, recoverable packet/metadata preservation, stale nonempty MP4 partials, incomplete-temp cleanup, complete manifest/MP4 temp retention, atomic destination preservation, one recording failure tick, finalization order, and first-fault retention across a second disk failure. Deliberately filling a host volume was avoided; standard ENOSPC injection was the safest practical low-space exercise.
- Both final Debug suites passed all five registered tests. The configured verifier preserved and inspected its 852-byte H.264 MP4, passed real WGC capture through NVIDIA H.264, PacketStore recovery, and FFmpeg MP4 writing, published fresh evidence, and the strict gate checker reported `Verified`.

Pass 143 startup-recovery evidence:

- `RecordingRecovery` performs a sorted, explicitly bounded scan on its own worker and reports complete, recoverable-prefix, incomplete-metadata, corrupt, and already-exported session states. Candidate facts include manifest version, complete/trailing packet bytes, packet count, final/temp paths, complete unpublished MP4 state, and the first retained scan error.
- Complete `session.json.tmp` files are parsed through the same version 1/2/3 manifest reader. Complete MP4 temps are structurally checked before atomic publication; otherwise recovery promotes valid metadata and remuxes the complete PacketStore prefix. Existing final outputs are never replaced. Failed actions keep the source candidate retryable, and discard moves it under `sessions\discarded` rather than deleting it.
- `RecordingRecoverySession` owns scan/export/discard worker lifetime and typed snapshots. `AppHost` posts state changes to the hidden window, logs useful candidate facts, and updates dynamic tray recovery/discard commands; the hidden-window thread performs no filesystem scan or mux work.
- A configured app startup scanned 14 real `%LOCALAPPDATA%\O'Louie\sessions` directories, classified 12 as already exported and two as complete/recoverable, logged packet/recovered-byte facts for both, remained responsive, and exited cleanly. The exercise intentionally did not mutate or discard those user sessions.
- Focused fixtures cover truncated packets, missing/current/legacy manifests, valid manifest and MP4 temps, corrupt packet headers, already-exported sessions, repeated scans, the scan limit, duplicate destinations, first-error retention, retry after action failure, reversible discard, and asynchronous state/notification transitions.
- A configured practical exercise appended a 16-byte incomplete tail to a valid two-packet H.264 session. Startup recovery classified the complete prefix, retained the durable session, and wrote `O'LouieRecordingRecoveryTests\exports\O'Louie-009-real-prefix.mp4` (852 bytes); `InspectMp4Artifact.ps1` confirmed `ftyp`, nonempty `mdat`, and `moov` boxes. Both Debug suites passed 5/5, the verifier republished its required artifact/report, and the strict checker reported `Verified`.

Pass 144 diagnostics evidence:

- The recorder worker publishes one copied diagnostics progress value per second through the existing state sink. It includes actual monitor/encoder identity, requested/negotiated/observed FPS and bitrate inputs, capture/accept/drop/encode counts, elapsed time and encoded bytes, configured/packet-bearing audio tracks, and bounded command/export queue counts. The UI owns no capture, encoder, PacketStore, export queue, or recovery object and creates no polling thread.
- `DiagnosticsSnapshot` merges recorder, recovery, and persisted settings into one immutable view, retains useful paths and the first actionable failure, and formats a selectable/copyable report. `AppHost` performs the merge only on its hidden-window thread after worker messages; hidden settings continue to render zero frames.
- The read-only Dear ImGui Diagnostics tab passed idle, active, saved, failed, recovery, transient outstanding-export, and repeated-open fixtures. Its final smoke rendered 10 frames while visible, remained at 10 while hidden, reopened to 12, confirmed `WDA_EXCLUDEFROMCAPTURE`, and produced `build\diagnostics-ui-smoke.bmp` with readable non-overlapping labels.
- A real configured recording kept Diagnostics visible while two clip exports ran. Low-frequency logs advanced to 805 captured/accepted/encoded frames with zero drops and then two saved exports with no failure. The two 950,320-byte clip MP4s and 1,570,693-byte full MP4 all passed structural inspection, the app stayed responsive, and normal exit succeeded. The controlled queue fixture, not the faster-than-one-second real muxes, proves the nonzero outstanding state.
- The final default native suite passed 4/4 and the configured suite passed 5/5. The verifier republished and inspected its 852-byte MP4/report, and the strict gate checker reported `Verified`.

Pass 145 long-session hardening and bounded-load evidence:

- The repository now owns a duration-guarded long-session runner, non-activating visual stimulus, targeted one-session recovery utility, settings backup/restore, scheduled live clip/bookmark commands, diagnostics/resource sampling, dual structural/`ffprobe` artifact inspection, partial checks, isolated recovery fixtures, and pre-existing-session comparison. Manual hotkey stops and console cancellation wait for save/failure before app exit.
- A real old-limit exercise reproduced `Prepared PCM queue is full` after about five minutes. Per-track queue capacity is now 1,024, bounded drain work is 64 blocks/tick, audio drains before video, and audio runtime/finalization faults retain recoverable packets. A seven-minute configured run under The Finals load stayed responsive, peaked at 42 queued blocks, kept both AAC tracks packet-bearing, saved six background exports plus the full MP4, passed both inspectors for all seven files, left no partial, and changed no pre-existing session.
- The user bounded the nominal 3,905-second run to 1,571.05 seconds. All 53 samples remained responsive; final live facts were 89,057 captured, 80,006 accepted, 80,002 encoded, 9,051 dropped, 116,004 encoded AAC packets, two packet-bearing AAC tracks, queue peak 32/1,024, and seven successful exports. A Settings open near the end explains the late process-resource step. This evidence is substantial but does not claim the required longer-than-one-hour duration.
- One burst clip failed locally because an earlier long H.264 sample overlapped the selected keyframe boundary. Adjacent exports and recording continued. Keyframe-aligned ranges now exclude pre-boundary H.264 samples while retaining overlapping AAC; focused tests cover the regression.
- Early runner cleanup interrupted full-file muxing and left an incomplete 1,218,183,216-byte temp. The targeted recovery utility selected only the new durable session, found 196,072 complete packets with zero trailing bytes, and produced a 1,629,056,904-byte H.264/two-AAC MP4. The recovered full file and seven successful exports passed `InspectMp4Artifact.ps1` and `ffprobe`, no partial remained, settings were restored byte-for-byte, and all 39 pre-existing session files were unchanged.
- Default and FFmpeg-configured Release builds/tests each passed 5/5. A fresh configured verifier artifact/report under `build-ffmpeg\ffmpeg-record-test-output-pass145` passed inspection and the strict checker reported `Verified`. Gyan's GPL-3.0 FFmpeg 8.1.2 package supplied standalone development-only `ffprobe`; it was not linked, staged, bundled, or substituted for the app's pinned LGPL-oriented dynamic FFmpeg root.

Pass 146 graceful external-stop evidence:

- The long-session runner now derives the latest recorder state from ordered log events, waits through a detected stopping state, and treats a terminal saved state as a successful `external-stop` exercise. It proceeds through normal log retention, MP4 inspection, partial checks, isolated recovery, pre-existing-session comparison, settings restoration, and process exit instead of throwing before evidence collection.
- `summary.json` distinguishes `Success` from `RequestedDurationCompleted` and records stop reason/time plus median endpoint-window resource trends. The default duration guard still rejects runs of one hour or less unless `-AllowShortRun` is explicit.
- Pure PowerShell tests cover latest-event state selection, prior-save/new-recording histories, terminal failures, flat/linear trends, and median resistance to endpoint outliers. The test is registered with CTest as `OLouieLongSessionToolTests`.
- A 120-second configured Release preflight was externally stopped at 37.18 seconds. Nine resource samples stayed responsive; four MP4s (full, two clips, one bookmark) passed `InspectMp4Artifact.ps1` and `ffprobe`; zero partials remained; the isolated recovery fixture exited 0; settings were restored byte-for-byte; pre-existing session difference count was zero; and no O'Louie process remained. Evidence is under `build-ffmpeg\long-session-early-stop-pass146-20260719-145536`. The summary explicitly reports `RequestedDurationCompleted: false`, so this does not claim the longer-than-one-hour requirement.
- Default and FFmpeg-configured Release builds/tests each passed 6/6, and `CheckFfmpegMuxerGate.ps1 -RequireVerified` still reported `Verified` for the Pass 145 real muxer artifact/report.

Pass 147 strict long-session evidence tooling:

- Runner evidence schema version 1 adds post-cleanup `cleanup.json` and `settings-after.json`, with pre/post settings SHA-256 values, owned app/stimulus exit state, and remaining O'Louie process count. The runner now waits after forced cleanup, fails a nominally successful invocation when cleanup verification fails, and always writes empty pre-existing-session differences as `[]` instead of losing the file to PowerShell's empty-pipeline behavior.
- `CheckLongSessionEvidence.ps1` independently reconciles retained summaries, CSVs, logs, artifact paths/sizes/statuses, queued export count, partial files, resource samples/trend coverage, diagnostics, command failures, recovery result, pre-existing-session inventories, settings hashes, and process cleanup. `Invalid` identifies inconsistent evidence; `Preflight` accepts internally consistent short/early-stop evidence without claiming the gate; `Verified` additionally requires requested/observed/resource durations greater than one hour, runner-owned start and stop, `ffprobe` evidence, and schema-1 cleanup proof. `-RequireCompletedDuration` exits nonzero for both invalid and preflight evidence.
- Complete-duration, short external-stop, and artifact-tamper fixtures run inside `OLouieLongSessionToolTests`. They prove the three status paths and prevent a summary-only claim from passing after an artifact changes. The retained Pass 146 run is accepted as legacy `Preflight`, while strict mode exits 1 and enumerates its duration/schema/cleanup gaps.
- This pass started no recording. Both Release suites passed 6/6 and the strict real FFmpeg muxer checker remained `Verified`. The required longer-than-one-hour configured run is still intentionally pending under the user's short-recording instruction.

Pass 148 schema-2 stream/configuration evidence:

- Runner evidence schema version 2 records the applied system-audio, microphone, separate-track, 48 kHz, and Media Foundation hardware-encoder settings. It parses configured/packet-bearing audio-track and submitted/saved/failed export counts from the final diagnostics line and stores structured `ffprobe` duration, format-size, codec, sample-rate, and channel facts for every artifact.
- The independent checker reconciles those settings and diagnostics with the summary, requires exactly one full recording, and verifies one H.264 video stream plus AAC audio in every MP4. `Verified` additionally requires the independently probed full MP4 to exceed one hour and contain both packet-bearing 48 kHz stereo AAC tracks. Schema-1 and older evidence remains valid as `Preflight` but cannot satisfy the duration gate.
- Complete-duration, short external-stop, missing-audio, and artifact-tamper fixtures pass. The missing-audio fixture proves that a nominally complete run with only one packet-bearing requested audio track remains `Preflight`; stale artifact metadata remains `Invalid`. The retained Pass 146 evidence is still accepted as legacy `Preflight`, and strict mode exits 1.
- No recording ran. Both Release suites passed 6/6, and `CheckFfmpegMuxerGate.ps1 -RequireVerified` still reported `Verified` for the Pass 145 artifact/report. The explicit longer-than-one-hour configured run remains pending under the user's short-recording instruction.

Pass 149 schema-3 artifact reinspection evidence:

- The runner now resolves and requires `ffprobe` before capture starts, preventing a long run that cannot produce strict codec/duration evidence. Schema 3 adds a SHA-256 digest to every artifact row while retaining the applied settings, final diagnostics, structured stream facts, cleanup hashes, and process-exit proof.
- The checker recomputes every artifact digest and fresh-runs both `InspectMp4Artifact.ps1` and `ffprobe` against each current MP4. It compares fresh duration, format size, H.264/AAC counts, sample rates, and channels with the cached runner row; schema-2 and older evidence can remain valid `Preflight` but cannot satisfy the duration gate.
- Focused fixtures now cover a same-size byte replacement and stale cached AAC counts in addition to complete, short, and missing-audio cases. Both new mutations are `Invalid`. The production reinspection boundary passed all four retained Pass 146 MP4s, freshly reporting one H.264 and one 48 kHz stereo AAC stream in each; that run remains `Preflight` because it is short and has only one packet-bearing audio source.
- No recording ran. Both Release suites passed 6/6, and the strict FFmpeg muxer checker remained `Verified` against the Pass 145 artifact/report. The explicit longer-than-one-hour configured run remains pending under the user's short-recording instruction.

Pass 150 independent resource-trend evidence:

- The checker now treats `resource-samples.csv` as the source of truth for process stability. It rejects missing, empty, negative, non-finite, non-monotonic, or nonresponsive samples and recomputes resource span, first/last/maximum working-set/private/handle metrics, and the complete five-sample median endpoint-window trend.
- Schema-3 summaries must retain and match the runner's endpoint/max facts and every recomputed trend property. Strict duration status uses the recomputed elapsed hours, and CLI growth output comes from the recomputed object, so a stale or edited summary cannot conceal sustained resource growth.
- Focused fixtures reject both a modified `WorkingSetGrowthBytes` summary and a negative handle-count sample while preserving the complete, short, missing-audio, stale-probe, and same-size artifact-tamper paths. The retained Pass 146 evidence remains valid `Preflight` with independently reproduced growth of 3,104,768 working-set bytes, 2,637,824 private bytes, one handle, and minus one thread.
- No recording ran. Both Release suites passed 6/6, and the strict FFmpeg muxer checker remained `Verified` against the Pass 145 artifact/report. The explicit longer-than-one-hour configured run remains pending under the user's short-recording instruction.

Pass 141 D3D11/hardware-encoder device-loss evidence:

- One shared classifier distinguishes D3D11 removed, reset, hung, driver-internal, and unknown-loss results while retaining both the failed operation HRESULT and `GetDeviceRemovedReason`. WGC texture creation/copy/callback polling, VideoProcessor initialization/view/blit, and Media Foundation DXGI sample submission/input/output/drain paths carry typed device details instead of reducing them to generic text.
- `BgraVideoRecordingSession` distinguishes D3D11 loss from hardware H.264 transform failure. That typed result survives the encode worker/bridge into the production recorder. The first failure stops capture once; queued dead-device video textures are counted and discarded; unaffected PCM/AAC is drained/flushed; already encoded packets proceed through manifest write, store close/recovery, and MP4 mux. A failure first observed during queue or encoder drain follows the same preservation path. The recorder reports the original subsystem and saved path without selecting a CPU encoder.
- Focused tests cover removed/reset/hung/driver/unknown classification, typed WGC latching, conversion-versus-transform classification, preservation disposition during queue and encoder drain, complete finalization order, and retention of the first error when a second finalization fault occurs. `dxcap.exe` was unavailable, so the host-wide TDR path was not forced; deterministic fault injection was used as the safest practical device-loss exercise.
- Default and FFmpeg-configured Debug suites each passed all five tests. The configured verifier exercised real WGC, D3D11 VideoProcessor conversion, NVIDIA H.264 DXGI submission/drain, packet recovery, and MP4 writing, then the strict checker reported `Verified`. A separate five-second production tray run saved a structurally valid 643,851-byte MP4 and exited cleanly.

Pass 139 enabled-but-silent audio evidence:

- The durable session manifest continues to record every configured source. Each individual export now includes only AAC tracks with real packets in its selected range and reports omitted configured track ids through `VideoExportPlan`/`ActiveRecordingClip`; it never invents AAC access units. Real AAC that starts late retains its positive offset from the keyframe-aligned video start.
- Focused recovered-plan tests cover an entirely absent manifest AAC track, a globally packet-bearing track absent from an early selected range, one late packet-bearing source plus one empty source, included/omitted-id overlap rejection, and codec mismatch preservation. Active clip and bookmark fixtures both succeed with a configured empty AAC source. The configured writer produced `h264-silent-audio.mp4` from valid H.264 plus an empty manifest AAC track; the 852-byte file passed the repository inspector and VLC H.264 playback to EOF without an audio stream.
- The final default and FFmpeg-enabled Debug suites each passed all five registered tests. The configured verifier regenerated its required MP4/report and the strict checker reported `Verified`.
- A real configured tray run recorded with system loopback enabled and no playback. Its version 3 manifest retained the 48 kHz stereo AAC track, while a direct `packets.dat` scan found 2,076 H.264 packets and zero AAC packets. During that same recording, a 1,901,141-byte clip and 3,590,108-byte bookmark clip finalized while capture continued; stop then saved a 5,914,782-byte full recording. All three passed `InspectMp4Artifact.ps1`, contained one H.264 track, opened through VLC `avcodec`, reached EOF, and had reported video durations of about 12.63, 23.69, and 40.29 seconds. No partial MP4 remained and the app exited through its tray command.

Hold line:

- Keep DXGI fallback, process-loopback audio, default mixed-track generation, general PCM resampling, installers/updaters/signing, and unrelated polish deferred unless a robustness requirement directly needs one of them.
