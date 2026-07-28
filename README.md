# O'Louie

O'Louie is a Windows-only C++20 screen recorder and clipping app. It records one selected monitor, saves keyframe-aligned clips and bookmarks without stopping the active recording, writes MP4 output, and stays quiet in the system tray when idle.

## Project Status

O'Louie is pre-release software. The first end-to-end recorder flow is implemented, including:

- tray and global-hotkey recording controls;
- Windows Graphics Capture video from one selected monitor;
- hardware-only Media Foundation H.264 encoding;
- direct WASAPI system-loopback and microphone capture with Media Foundation AAC encoding;
- full-session MP4 export, live clip export, bookmarks, persisted settings, diagnostics, and interrupted-session recovery; and
- focused automated tests plus manual capture, encode, audio, UI, recovery, and long-session verification tools.

Recording and MP4 export require an FFmpeg-enabled build. A default build intentionally compiles and tests the non-FFmpeg boundary, but recording start fails before capture because no MP4 muxing backend is configured.

The explicit longer-than-one-hour validation gate, nonzero bookmark post-roll, process-specific audio, audio mixing and sample-rate conversion, preview, seamless display-resize recovery, installer/updater/signing, and release packaging remain incomplete. See [Intentional Deferrals](#intentional-deferrals) and `docs/progress.md` for details.

## Build Requirements

- A 64-bit Windows development environment.
- CMake 3.26 or newer.
- Visual Studio 2022 or a compatible MSVC C++20 toolset with the Windows SDK.
- Windows PowerShell 5.1 or PowerShell 7 for the repository build and verification scripts.
- Network access for the first Dear ImGui download, or an already prepared pinned Dear ImGui source tree supplied through `OLOUIE_IMGUI_ROOT`.

A compatible Media Foundation hardware H.264 encoder and Windows Graphics Capture support are runtime requirements for actual recording. FFmpeg source-build prerequisites are needed only when using the repository's optional pinned FFmpeg build helper.

## Basic Build

The root entrypoint prepares or validates pinned Dear ImGui `v1.92.8`, configures a Visual Studio x64 build, builds it, and runs the tests:

```powershell
.\build.ps1 -Configuration Debug
```

For a direct CMake build from a fresh checkout, prepare Dear ImGui first:

```powershell
.\tools\PrepareDearImGui.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOLOUIE_BUILD_TESTS=ON -DOLOUIE_ENABLE_FFMPEG=OFF
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The build intentionally targets Windows only. CMake fails early on non-Windows platforms.

## External Dependencies

- **Windows platform APIs:** Win32, Direct3D 11/DXGI, Windows Graphics Capture, Media Foundation, WASAPI, and related Windows SDK libraries. These are not vendored.
- **Dear ImGui `v1.92.8`:** the MIT-licensed core plus official Win32 and DirectX 11 backends are downloaded into ignored `_deps\imgui` and compiled into O'Louie. The archive URL and SHA-256 are pinned in `tools/PrepareDearImGui.ps1` and recorded in `docs/dear_imgui_provenance.md`.
- **FFmpeg `8.1.2`:** optional at configure time but required for the current MP4 recorder path. O'Louie dynamically links `libavformat`, `libavcodec`, `libavutil`, and `libswresample`. The repository helper builds an LGPL-oriented shared configuration with GPL, nonfree, and version3 components disabled; externally supplied FFmpeg roots must be reviewed independently.
- **FFmpeg source-build tools:** `gpg`, `tar`, MSYS2 `bash`/`make`/`nasm`/`cmp`, and the MSVC x64 tools are required only for `tools/BuildFfmpegLgplFromSource.ps1`.
- **`ffprobe`:** optional development-only artifact inspection; it is not linked into or shipped by the application.

See `THIRD_PARTY_NOTICES.md` before distributing source or binaries. No release package is currently produced.

## Run The Recorder

Build with the configured local FFmpeg root, then launch the tray app:

```powershell
cmake -S . -B build-ffmpeg -G "Visual Studio 17 2022" -A x64 -DOLOUIE_BUILD_TESTS=ON -DOLOUIE_ENABLE_FFMPEG=ON -DOLOUIE_FFMPEG_ROOT=.\_deps\ffmpeg
cmake --build build-ffmpeg --config Debug
& ".\build-ffmpeg\Debug\O'Louie.exe"
```

Open **Settings** from the tray menu or by double-clicking the tray icon. Valid settings are stored at `%LOCALAPPDATA%\O'Louie\settings\settings.json`; malformed, invalid, and unsupported files are diagnosed and fall back safely without silently overwriting unsupported data. Use `Ctrl+Shift+R` or the tray menu's **Start Recording** command to begin. While recording, use the configured preset/bookmark hotkeys or the dynamic tray commands without stopping capture. Bookmark commands currently support configurable pre-roll with zero post-roll, and the durable marker is written to the session manifest. Use the recording hotkey again or **Stop and Save Full Recording** to stop and finalize. MP4 files are written to the configured output folder; recoverable session data remains under `%LOCALAPPDATA%\O'Louie\sessions`. Saved files contain H.264 video plus each enabled direct AAC track that emitted packets in the selected export range. Empty configured sources remain in `session.json` but do not create empty MP4 streams or require synthetic AAC silence. Current defaults record system loopback and leave microphone capture disabled. A default build without FFmpeg still compiles and tests, but recording start fails explicitly before capture because no MP4 backend is configured.

On the first rebranded launch, an existing legacy runtime-data root is moved atomically to `%LOCALAPPDATA%\O'Louie` before settings or recovery data are opened. If both roots already exist they are left untouched and O'Louie reports the conflict; if the move is temporarily blocked, the legacy root remains active for that run and migration is retried later. Supported saved output paths and valid moved recovery manifests are rebased without changing their schema versions.

The Audio settings tab can monitor the current Windows default microphone through headphones or speakers. Choose **Windows Default** or a specific saved playback device, then toggle **Start Mic Check**. A missing saved device falls back to the current Windows default without discarding the saved ID; no available output is reported explicitly. Mic check is independent of the recording microphone checkbox, stops when Settings closes, and is mutually exclusive with active recording.

The Video settings tab offers **Balanced** and **Capture First** recording priority. Both use thread-scoped Windows multimedia scheduling for recorder, video capture/encode, and live system/microphone audio workers. Capture First requests higher relative multimedia priority plus low-latency/faster hardware-encoder tuning; it can trade some encoder quality for speed and slightly reduce in-game frame rate under load. O'Louie never raises the process to real-time priority, and copied diagnostics report the scheduling results and the codec values the driver actually accepted.

Optional FFmpeg backend configuration boundary:

```powershell
# Combined archive:
.\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath C:\Downloads\ffmpeg-lgpl-shared-dev.zip -DestinationDir .\_deps\ffmpeg

# Split development/runtime archives:
.\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath C:\Downloads\ffmpeg-lgpl-dev.zip,C:\Downloads\ffmpeg-lgpl-shared.zip -DestinationDir .\_deps\ffmpeg

# Inspect an archive set without preparing _deps\ffmpeg:
.\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath C:\Downloads\ffmpeg-lgpl-dev.zip,C:\Downloads\ffmpeg-lgpl-shared.zip -InspectOnly
.\tools\CheckFfmpegMuxerGate.ps1 -FfmpegArchivePath C:\Downloads\ffmpeg-lgpl-dev.zip,C:\Downloads\ffmpeg-lgpl-shared.zip

.\tools\CheckFfmpegMuxerGate.ps1
.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot .\_deps\ffmpeg

# Require a previous successful verifier report/artifact when checking status:
.\tools\CheckFfmpegMuxerGate.ps1 -FfmpegRoot .\_deps\ffmpeg -RequireVerified

# Or prepare _deps\ffmpeg and run the muxer verifier from archive input:
.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegArchivePath C:\Downloads\ffmpeg-lgpl-dev.zip,C:\Downloads\ffmpeg-lgpl-shared.zip

# The verifier preserves and inspects O'LouieRecordTests\exports\h264.mp4 under build-ffmpeg\ffmpeg-record-test-output by default,
# then atomically publishes verification-report.txt next to the preserved artifact after all requested verification steps succeed.
```

Official pinned-source local build path:

```powershell
# Dry-run the sanctioned source-build plan without downloading or building:
.\tools\BuildFfmpegLgplFromSource.ps1 -DescribePlan

# Check local build prerequisites without downloading, extracting, building, or writing files:
.\tools\BuildFfmpegLgplFromSource.ps1 -CheckPrerequisites

# Download only the pinned official FFmpeg source release and .asc signature from ffmpeg.org,
# require local GPG signature verification, build local Windows x64 shared libraries,
# verify the resulting root, and write docs\ffmpeg_source_build_provenance.md:
.\tools\BuildFfmpegLgplFromSource.ps1 -Force

.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot .\_deps\ffmpeg
.\tools\CheckFfmpegMuxerGate.ps1 -FfmpegRoot .\_deps\ffmpeg -RequireVerified
```

The source-build helper defaults to the pinned official FFmpeg 8.1.2 source archive at `https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz` plus `https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz.asc`. It refuses non-`ffmpeg.org` source URLs, does not download binary bundles, uses local `gpg`, `tar`, MSYS2 `bash`/`make`/`nasm`/`cmp`, and MSVC x64 tools, builds shared libraries with GPL, nonfree, and version3 components disabled, normalizes FFmpeg's MSVC import libraries from the upstream `bin` install location into the repository's expected `lib` layout, and leaves the result in the ignored development-only `_deps\ffmpeg` root. Codex may install or download the missing local prerequisites needed to run this helper, then rerun the prerequisite check and source build automatically. `tar`, `gpg`, MSYS2 `bash`, and MSVC `vcvarsall.bat` can be supplied explicitly with `-TarPath`, `-GpgPath`, `-BashPath`, and `-VcVarsAllPath` when those tools are installed outside `PATH`. If `-DestinationDir` or `-WorkDir` are overridden, they must be separate child directories under `_deps`; the helper will not generate, replace, or delete source-build directories elsewhere in the repository. `-CheckPrerequisites` reports missing local tools and exits nonzero when the source build cannot run yet, without downloading or writing files. The generated provenance document is a local development record, not permission to bundle or redistribute FFmpeg.

Tooling-only self-test for the FFmpeg gate helpers:

```powershell
.\tools\TestFfmpegGateTools.ps1
```

This self-test is also registered with CTest when `OLOUIE_BUILD_TESTS=ON`, so the normal `.\build.ps1 -Configuration Debug` path protects the gate helper scripts.

The configured long-session runner backs up and restores settings, schedules live clips/bookmarks, samples resources, inspects every MP4 with the repository inspector and `ffprobe` when available, runs an isolated recovery fixture, and checks that pre-existing sessions did not change:

```powershell
.\tools\TestLongSessionExerciseTools.ps1
.\tools\RunLongSessionExercise.ps1 -BuildDirectory .\build-ffmpeg -Configuration Release
.\tools\CheckLongSessionEvidence.ps1 -EvidenceDirectory .\build-ffmpeg\long-session-<timestamp> -RequireCompletedDuration
```

The runner rejects durations of one hour or less by default so a preflight cannot be mistaken for the duration gate; use `-AllowShortRun` only for an intentional short exercise. It requires `ffprobe` before capture starts, so an hour cannot be spent producing evidence that strict verification cannot inspect. A successful external recording stop still completes artifact inspection and writes `StopReason: external-stop` plus `RequestedDurationCompleted: false` in `summary.json`. It also publishes `cleanup.json` and `settings-after.json` after settings restoration and owned-process cleanup. Evidence schema 3 retains the applied system-audio, microphone, separate-track, sample-rate, and hardware-encoder settings; reconciles configured/packet-bearing audio and export counts with final diagnostics; and records SHA-256 plus structured `ffprobe` duration, size, codec, sample-rate, and channel facts for every MP4. `CheckLongSessionEvidence.ps1` hashes each current artifact, reruns the repository MP4 inspector and `ffprobe`, validates every resource sample, and recomputes endpoint/max metrics plus the median-window resource trend instead of trusting cached summary values. It reports `Verified` only when the recording and recomputed resource series exceed one hour, runner-owned start/stop and cleanup are proven, every artifact contains H.264 plus AAC, and the full MP4 contains both 48 kHz stereo AAC tracks; `-RequireCompletedDuration` makes any non-verified result fail.

Or point the build at an existing local root:

```powershell
.\tools\VerifyFfmpegRoot.ps1 -FfmpegRoot C:\path\to\ffmpeg-lgpl
.\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot C:\path\to\ffmpeg-lgpl
.\build.ps1 -Configuration Debug -EnableFfmpeg -FfmpegRoot C:\path\to\ffmpeg-lgpl
```

Equivalent direct CMake path:

```powershell
cmake -S . -B build-ffmpeg -G "Visual Studio 17 2022" -A x64 -DOLOUIE_BUILD_TESTS=ON -DOLOUIE_ENABLE_FFMPEG=ON -DOLOUIE_FFMPEG_ROOT=C:\path\to\ffmpeg-lgpl
cmake --build build-ffmpeg --config Debug
ctest --test-dir build-ffmpeg -C Debug --output-on-failure
```

The expected FFmpeg root is an LGPL dynamic build with `include\libavformat\avformat.h`, `include\libavcodec\avcodec.h`, `include\libavutil\avutil.h`, `include\libswresample\swresample.h`, import libraries for `avformat`, `avcodec`, `avutil`, and `swresample` under `lib`, and matching runtime DLLs under `bin`. If those folders are split, pass all three of `-FfmpegIncludeDir`, `-FfmpegLibraryDir`, and `-FfmpegBinaryDir` to `build.ps1`, or set `OLOUIE_FFMPEG_INCLUDE_DIR`, `OLOUIE_FFMPEG_LIBRARY_DIR`, and `OLOUIE_FFMPEG_BINARY_DIR` for CMake. `tools\PrepareFfmpegRootFromArchive.ps1` can unpack one combined ZIP archive, or multiple ZIP archives that together provide `include`, `lib`, and `bin`, into the ignored local `_deps\ffmpeg` root and then run the root verifier. Its `-InspectOnly` mode extracts to a temporary ignored folder, reports whether the archive set contains a complete root or split components, and removes the temporary files without preparing `_deps\ffmpeg`. This archive-preparation helper itself does not download, install, or license-vet FFmpeg; Codex may automatically acquire and vet suitable inputs before invoking it, or use the preferred official pinned-source build path. `tools\CheckFfmpegMuxerGate.ps1` reports whether the current FFmpeg muxer gate is pending, ready, or verified; `-FfmpegArchivePath` can inspect candidate ZIP archives but still reports pending until a root is prepared and muxer verification runs, it is exclusive with `-FfmpegRoot`, split include/lib/bin status inputs, or `-RunVerification`, and partial explicit split-directory input fails as incomplete instead of being treated as an absent root. Its `-RunVerification` mode forwards `-RunWgcSmoke`, `-WgcDurationMs`, `-Width`, `-Height`, `-Fps`, and `-BitrateMbps` to `tools\VerifyFfmpegMuxer.ps1` for prepared roots or explicit split include/lib/bin directories; if no prepared root or complete split-directory input is available, `-RunVerification` exits nonzero instead of reporting a passive Pending status. WGC smoke options on the gate checker require `-RunVerification` so status-only checks do not silently ignore them. When a root is ready, it validates the default or supplied `-RecordTestOutputDir`/`-VerificationReportPath` evidence targets, including parent-directory chain shape, then checks that the schema-versioned timestamped success report contains no duplicate keys, malformed non-empty lines, or unexpected schema-version-1 keys, names `tools\VerifyFfmpegMuxer.ps1` as verifier, names the same build configuration and FFmpeg root or split directories currently being checked, includes the `ffmpeg_archive_input` field, includes well-formed WGC smoke status/options evidence, the preserved MP4 artifact is present, the report timestamp is not older than the artifact and not more than 5 minutes in the future, the report's artifact size and last-write UTC match the current file, and that artifact passes `tools\InspectMp4Artifact.ps1`, it reports `Verified`. Root-only and split-directory verifier runs write `ffmpeg_archive_input: <not supplied>`; archive-preparation verifier runs record the archive path set, and missing or empty archive-input fields are invalid. `-RequireVerified` turns missing, incomplete, stale, backdated, future-dated, legacy-format, wrong-configuration, unexpected-verifier, invalid-WGC-smoke, malformed-WGC-options, duplicate-key, malformed-line, unexpected-key, or mismatched report evidence into a nonzero status for gate checks. `tools\TestFfmpegGateTools.ps1` uses synthetic ZIP fixtures to validate the local helper scripts, expected layout, archive inspection, archive candidate gate reporting, verifier archive-input failure routing, verifier WGC/report option validation, muxer verifier stale-report cleanup, muxer verifier report-directory target rejection, muxer verifier record-output file target rejection, muxer verifier report/record-output same-path rejection, muxer verifier evidence-target parent/ancestor-file rejection, gate checker evidence-target path validation, gate checker WGC run-verification option validation, gate run-verification input requirement reporting, MP4 artifact box/payload inspection, verification-report evidence recognition, verification-report schema/provenance/configuration/timestamp/future-timestamp/archive/duplicate-key/malformed-line/unexpected-key/WGC-smoke option-schema detail checks, root/split FFmpeg-input consistency checks, partial split-directory rejection, gate-status and muxer-verifier archive/root input conflict rejection, gate archive run-verification conflict reporting, archive-input field presence/empty/root-only semantics, artifact metadata consistency checks, root and split-directory CMake FFmpeg option wiring, and explicit failure behavior for incomplete roots; it does not validate real FFmpeg binaries, linkability, licensing, or MP4 muxing. `tools\VerifyFfmpegRoot.ps1` checks the local layout without downloading, installing, or bundling FFmpeg. `tools\VerifyFfmpegMuxer.ps1` runs the root verifier, builds `build-ffmpeg`, runs FFmpeg-enabled tests, and can run the manual WGC smoke with `-RunWgcSmoke`; it rejects nonpositive `-WgcDurationMs`, `-Width`, `-Height`, `-Fps`, and `-BitrateMbps` values before preparing archives, building, running tests, or writing a success report. After static verifier options, a directory-valued record output target whose parent chain is not blocked by an existing file, a file-valued report target whose parent chain is not blocked by an existing file, and distinct report/output target paths are accepted, it removes any existing report at the target `verification-report.txt`/`-VerificationReportPath` before archive preparation, root verification, build, or test work starts, so a failed rerun does not leave stale success evidence at that path. When passed `-FfmpegArchivePath`, it first prepares `_deps\ffmpeg` from the local archive set through the same local archive preparer, and archive preparation input is exclusive with `-FfmpegRoot` or split include/lib/bin verifier inputs. Its direct `O'LouieRecordTests` run preserves output under `build-ffmpeg\ffmpeg-record-test-output` by default, or under a directory-valued `-RecordTestOutputDir` when supplied, requires the FFmpeg-backed video-only artifact `O'LouieRecordTests\exports\h264.mp4` to exist, runs `tools\InspectMp4Artifact.ps1` to verify top-level `ftyp`, `mdat`, and `moov` MP4 boxes plus nonempty media-data and movie-metadata payloads, and atomically publishes a success-only `verification-report.txt` by writing through a same-directory temporary file before replacing the final path; the report includes schema version, verifier provenance, FFmpeg archive input, artifact path, byte size, last-write UTC, and WGC smoke status/options beside the preserved output by default or to a file-valued `-VerificationReportPath` distinct from `-RecordTestOutputDir` when supplied. Configured builds copy the selected FFmpeg DLLs beside each executable. Default builds do not bundle or silently fall back to FFmpeg.

When `tools\CheckFfmpegMuxerGate.ps1` reports a prepared root or complete split-directory input as ready, its suggested verifier command preserves caller-supplied `-RecordTestOutputDir` and `-VerificationReportPath` values so the next run writes evidence to the locations the gate checker inspected.

`tools\PrepareFfmpegRootFromArchive.ps1 -DestinationDir <dir>` must name a directory inside this repository. `-Force` may replace an existing destination directory, but file-valued destinations and destination parent chains blocked by an existing file are rejected before archive extraction.

When `tools\VerifyFfmpegMuxer.ps1 -FfmpegArchivePath <archive.zip>` is paired with `-FfmpegArchiveDestinationDir <dir>`, the verifier applies that same destination validation before removing stale verification reports or invoking archive preparation.

`-FfmpegArchiveDestinationDir` and `-ForceFfmpegArchivePreparation` are valid only with `tools\VerifyFfmpegMuxer.ps1 -FfmpegArchivePath`; the verifier rejects them before stale-report cleanup when no archive input is supplied.

Manual capture smoke, after building:

```powershell
& ".\build\Debug\O'LouieCaptureSmoke.exe"
& ".\build\Debug\O'LouieCaptureSmoke.exe" --wgc 3000
```

The first command enumerates monitors, creates a D3D11 device, smoke-tests app-owned BGRA source plus NV12 output texture allocation, initializes the reusable D3D11 VideoProcessor BGRA-to-NV12 converter, and executes a blit for the future GPU conversion path. The second command explicitly starts a short Windows Graphics Capture monitor smoke and counts frames/timestamps without encoding or saving output. The WGC path is manual so normal builds/tests do not trigger screen-capture consent prompts.

Manual encoder smoke, after building:

```powershell
& ".\build\Debug\O'LouieEncodeSmoke.exe"
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-probe 1920 1080 60 20
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-session 1920 1080 60 20
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-submit 1920 1080 60 20
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-bgra-submit 1920 1080 60 20
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-bgra-submit 1920 1080 60 20 --capture-first
& ".\build\Debug\O'LouieEncodeSmoke.exe" --h264-wgc-submit 3000 1920 1080 60 20
```

The first two commands validate the requested H.264 settings and enumerate Media Foundation hardware H.264 encoder MFTs for NV12 input and H.264 output. The third command creates a D3D11 device for the primary monitor, activates the selected hardware MFT, attaches a DXGI device manager when the transform is D3D11-aware, applies basic bitrate/FPS/GOP settings where accepted, configures H.264 output plus NV12 input media types, and reports transform traits. The fourth command additionally creates generated NV12 D3D11 textures, wraps them as MF DXGI samples, routes a short monotonic frame sequence through the synthetic video recording session boundary, drains available output between submissions, reports encoded packet metadata including Annex B/SPS/PPS/IDR detection, SPS/PPS payload sizes, and derived AVCC extradata size, then appends the drained packets to a temporary H.264 `PacketStore`. The generated-BGRA commands route textures through the reusable GPU BGRA-to-NV12 converter and caller-owned NV12 H.264 submit path, drain packets, and append them to a temporary H.264 `PacketStore`; adding `--capture-first` also requests low latency and fastest quality-versus-speed tuning and prints support plus accepted-value readback. The final command bootstraps the current video runtime, routes video preflight/setup/run through `VideoRecordingRunSession`, starts a bounded WGC monitor frame-copy smoke, copies a small batch into app-owned BGRA textures, routes those copied frames through the same BGRA-to-H.264 path, appends encoded packets to a temporary H.264 `PacketStore`, builds ready video metadata, writes and reads `session.json`, closes the store, recovers `packets.dat`, builds a video-only export plan, validates a video MP4 mux request with packet-file path plus H.264 dimensions/FPS/SPS/PPS/AVCC metadata, reports FFmpeg backend availability, validates FFmpeg MP4 video stream setup when configured, dry-runs payload reads from `packets.dat`, and attempts the configured-only video MP4 write. Default builds report the mux backend as unavailable instead of writing output.

Manual audio smoke, after building:

```powershell
& ".\build\Debug\O'LouieAudioSmoke.exe"
& ".\build\Debug\O'LouieAudioSmoke.exe" --loopback 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --mic 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --capture-sources 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --live-sources 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --live-capture-encode 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --capture-encode 1000
& ".\build\Debug\O'LouieAudioSmoke.exe" --audio-session 1000
```

The first command enumerates render/capture endpoints. The second command runs a short system-loopback smoke and counts PCM packets/frames without encoding or saving audio. The third command does the same for the default microphone endpoint.
The fourth command builds the current audio source plan, runs supported system-loopback and microphone sources through the manager-driven sink handoff smoke, and reports deferred source work explicitly.
The fifth command starts live default WASAPI system-loopback and microphone sources against a caller-owned sink, waits for the requested duration, stops them, and reports packets/frames without encoding or persistence.
The sixth command builds a temporary `PacketStore`, initializes real Media Foundation AAC encoders for currently supported direct source tracks, starts live default WASAPI sources through the capture-to-encode bridge, ticks bounded encode draining while capture runs, stops sources, flushes, and reports the packet store path.
The seventh command uses the older duration-bounded source smoke path with real AAC encoders, then flushes the session and reports the packet store path. Sample-rate conversion is still deferred, so unsupported capture input formats fail explicitly instead of being silently converted.
The eighth command runs the recorder-facing audio-only session owner: preflight direct audio tracks, create a caller-owned temporary `PacketStore`, prepare AAC encoders/session binding, run live capture/encode for the requested duration, drain, flush, and report the packet store path.

Manual settings-window smoke, after building:

```powershell
& ".\build\Debug\O'LouieSettingsUiSmoke.exe" .\build\settings-ui-smoke.bmp
```

This opens the real Dear ImGui Win32/D3D11 host on the Diagnostics tab, verifies `WDA_EXCLUDEFROMCAPTURE`, writes a visual artifact after temporarily clearing exclusion only inside the smoke process, confirms that rendered-frame count remains unchanged after the window is hidden, and reopens the same host to verify repeated-open lifetime. `O'LouieRuntimeControl` is a developer-only helper used to repeat real tray-command settings/recording verification against a running app; `open-diagnostics` opens the live Diagnostics tab.

## Current Layout

- `src/app`: process lifetime, single-instance ownership, persisted-settings load/apply coordination, settings-to-recorder configuration, tray/hotkey recording and recovery command routing, and worker-state notification marshalling.
- `src/win32`: hidden message window, dynamic tray recording/recovery commands and states, transactional global-hotkey registration, current-user startup registration, and capture-exclusion helpers.
- `src/logging`: runtime data folders and file logging.
- `src/settings`: defaults, validation, hotkey parsing, and versioned atomic `settings.json` persistence.
- `src/graphics`: monitor enumeration, D3D11/DXGI device ownership, BGRA-to-NV12 planning, texture allocation smoke, reusable D3D11 VideoProcessor conversion ownership, and future GPU processing ownership.
- `src/capture`: manual WGC smoke ownership, stoppable long-running WGC monitor-session ownership, target-FPS cadence filtering before GPU copy, bounded app-owned BGRA frame handoff, reusable owned-video-frame queue ownership, and future DXGI fallback ownership.
- `src/diagnostics`: immutable recorder/recovery/settings snapshot composition, encoded observed-FPS/bitrate calculation, separate cadence-limit and queue-drop counts, first-failure selection, and copyable report formatting.
- `src/audio`: audio endpoint enumeration, PCM packet/timestamp shaping, audio track planning, reusable audio capture/AAC preflight and session assembly, explicit long-running source start/tick/stop/drain/flush ownership, production recorder integration, same-rate PCM preparation, Media Foundation AAC encoding, AAC-to-`PacketStore` handoff, and future process-loopback, default mixing, and sample-rate conversion.
- `src/encode`: hardware-only Media Foundation H.264 ownership with bounded input-sample retention, GPU BGRA-to-NV12 conversion, bounded frame queue composition, a dedicated event-driven video encode worker, cached nonblocking runtime snapshots, final encoder draining, video bootstrap/metadata ownership, and retained manual smoke paths.
- `src/record`: shared session-clock/timebase conversion, bounded asynchronous encoded-packet writing with ordered durability barriers, storage recovery and live export snapshots, packet-truthful H.264/AAC/bookmark session manifests and export plans, typed disk/write diagnostics, recovery-preserving atomic publication, bounded background clip/bookmark export, FFmpeg MP4 writing, ordered long-running cross-media recorder ownership, and bounded asynchronous startup recovery/export/discard ownership.
- `tools`: repository-owned developer verification helpers, including pinned Dear ImGui acquisition, local FFmpeg archive/source preparation, gate-helper self-tests, muxer-gate status reporting, root/muxer verification, bounded long-session exercise, focus-independent visual stimulus, runtime commands, and targeted interrupted-session recovery.
- `src/ui`: event-driven Dear ImGui Win32/D3D11 host, the General, Video, Audio, Clips/Bookmarks, and Hotkeys editor, and the read-only Diagnostics view.
- `tests`: focused shell, settings persistence/hotkeys/UI lifetime, diagnostics snapshots, recording, recovery classification/actions/state, audio, encoding, muxing, pipeline-ordering, cleanup, cancellation, and backend-boundary tests.
- `docs/progress.md`: pass-by-pass progress against the implementation milestones.

## Intentional Deferrals

Not implemented in this pass:

- DXGI Desktop Duplication.
- PCM sample-rate conversion/resampling for AAC input preparation.
- Default mixed audio track generation/mixing.
- Nonzero bookmark post-roll scheduling.
- Notification overlay rendering.
- Installer, updater, signing, or distribution packaging.

Windows Graphics Capture now has both retained duration-bounded smoke paths and a stoppable long-running monitor session used by the tray recorder. The production recorder preflights requested WASAPI sources and all encoders, derives video and audio timestamps from one monotonic origin, signals a dedicated video encode worker for every admitted frame, drains direct audio independently, and queues H.264/AAC packets to one bounded storage worker. It saves configured active clips and bookmark clips through one bounded background mux queue without stopping capture, persists bookmark markers, then stops both input families before ordered queue/encoder/storage draining and saves the full MP4. Export planning keeps real late-audio offsets and omits configured AAC tracks only when the selected range has no packets. A selected-monitor resize/disconnect, classified D3D11/hardware-H.264 runtime failure, or live PacketStore write failure stops the fixed-dimension session explicitly, preserves recoverable packets and metadata where possible, names the failed subsystem/operation/path, and never switches monitors or falls back to a CPU encoder. Manifest and MP4 publication retain the old destination plus a complete temp on atomic-publish failure while incomplete temps are cleaned. Startup recovery classifies durable sessions and unpublished temps, remuxes complete packet prefixes, protects existing MP4s, and keeps discard reversible. One copied low-frequency diagnostics snapshot exposes recorder/recovery state, monitor and encoder identity, requested/negotiated/observed rates, frame, latency, writer-pressure, and export counters, configured and packet-bearing audio tracks, paths, and the first actionable failure without giving the UI access to capture or encoder objects or waiting on the video processing lock. Shorter export-under-load, interrupted-finalization recovery, and externally stopped runner verification are complete. Preview, seamless resize/device recreation, process audio, default mixing, nonzero bookmark post-roll, the explicit longer-than-one-hour duration run, and packaging remain deferred.

The app shell is tray-first. Dear ImGui renders at about 30 FPS only while settings are visible and performs no hidden ImGui frames. Its pinned source is an ignored development dependency with recorded provenance. FFmpeg source/build roots and all configured build output are ignored; FFmpeg-enabled builds copy selected runtime DLLs beside their executables, but no distribution package is currently produced. WIL and other optional third-party dependencies remain deferred until a subsystem needs them.

## Design References

The clip recorder design and implementation references are:

- `clip_recorder_design.txt`
- `clip_recorder_implementation_plan.txt`
- `docs/implementation_gates.md`
- `docs/progress.md`

Those files define the Windows-only C++20/CMake direction, D3D11/DXGI/WGC capture direction, hardware H.264-only recording direction, MP4 output direction, WASAPI audio direction, Dear ImGui settings direction, and the low-idle-overhead tray-first runtime behavior.

## Licensing

No project license has been selected or added. Third-party components remain subject to their own license terms; see `THIRD_PARTY_NOTICES.md`. That notice file does not grant a license to O'Louie itself.
