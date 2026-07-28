#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioCaptureEncodeBridge.h"
#include "audio/AudioCaptureManager.h"
#include "audio/AudioEncodeSession.h"
#include "audio/WasapiCaptureSource.h"

namespace olouie::audio {

class IAudioLiveCaptureSource {
 public:
  virtual ~IAudioLiveCaptureSource() = default;

  virtual CapturedAudioSource source() const noexcept = 0;
  virtual WasapiCaptureSourceResult Start(ICapturedPcmSink* sink) = 0;
  virtual void Stop() = 0;
  virtual bool IsRunning() const noexcept = 0;
  virtual PcmCaptureStats SnapshotStats() const = 0;
  virtual WasapiCaptureSourceResult LastResult() const = 0;
};

using AudioLiveCaptureSourceFactory =
    std::unique_ptr<IAudioLiveCaptureSource> (*)(
        const AudioCaptureSourceBinding& binding);

struct AudioLiveCaptureEncodeOptions {
  std::chrono::milliseconds duration{0};
  std::chrono::milliseconds drain_interval{10};
  size_t max_blocks_per_drain_tick = 16;
  int64_t qpc_origin_ns = 0;
  bool maintain_track_continuity = false;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
};

enum class AudioLiveCaptureEncodeStatus {
  Success,
  InvalidConfig,
  SourceStartFailed,
  CaptureFailed,
  DrainFailed,
  FlushFailed,
};

struct AudioLiveCaptureEncodeSourceResult {
  CapturedAudioSource source;
  AudioCaptureSourceRuntime runtime =
      AudioCaptureSourceRuntime::SystemLoopback;
  AudioCaptureSourceSupport support = AudioCaptureSourceSupport::Deferred;
  uint32_t track_id = 0;
  bool attempted = false;
  bool started = false;
  bool stopped = false;
  WasapiCaptureSourceResult start_result;
  WasapiCaptureSourceResult final_result;
  PcmCaptureStats capture;
  uint64_t synthetic_silence_packet_count = 0;
  uint64_t synthetic_silence_frame_count = 0;
  uint64_t retimed_packet_count = 0;
  std::wstring message;
};

struct AudioLiveCaptureEncodeResult {
  bool bridge_configured = false;
  bool deferred_mixed_track = false;
  size_t attempted_source_count = 0;
  size_t started_source_count = 0;
  size_t deferred_source_count = 0;
  uint64_t packet_count = 0;
  uint64_t frame_count = 0;
  uint32_t drain_tick_count = 0;
  uint64_t drained_block_count = 0;
  std::vector<AudioLiveCaptureEncodeSourceResult> sources;
  AudioEncodeSessionResult last_tick_drain;
  AudioEncodeSessionResult final_drain;
  AudioEncodeSessionResult flush;
  CapturedPcmSessionSinkStats sink_stats;
  AudioSourceSessionDispatchResult last_dispatch;
};

struct AudioLiveCaptureEncodeRunResult {
  AudioLiveCaptureEncodeStatus status =
      AudioLiveCaptureEncodeStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

std::unique_ptr<IAudioLiveCaptureSource> CreateDefaultAudioLiveCaptureSource(
    const AudioCaptureSourceBinding& binding);

class AudioLiveCaptureEncodeSession final {
 public:
  AudioLiveCaptureEncodeSession(
      AudioTrackPlan plan,
      AudioLiveCaptureEncodeOptions options,
      AudioEncodeSession* session,
      AudioLiveCaptureSourceFactory source_factory = nullptr);
  ~AudioLiveCaptureEncodeSession();

  AudioLiveCaptureEncodeSession(const AudioLiveCaptureEncodeSession&) =
      delete;
  AudioLiveCaptureEncodeSession& operator=(
      const AudioLiveCaptureEncodeSession&) = delete;

  AudioLiveCaptureEncodeRunResult Prepare();
  AudioLiveCaptureEncodeRunResult Start();
  AudioLiveCaptureEncodeRunResult DrainTick();
  AudioLiveCaptureEncodeRunResult StopSources();
  AudioLiveCaptureEncodeRunResult DrainQueuedBlocks();
  AudioLiveCaptureEncodeRunResult FlushEncoders();

  bool IsPrepared() const noexcept;
  bool IsRunning() const noexcept;
  const AudioLiveCaptureEncodeResult& result() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

AudioLiveCaptureEncodeRunResult RunAudioLiveCaptureEncode(
    const AudioTrackPlan& plan,
    const AudioLiveCaptureEncodeOptions& options,
    AudioEncodeSession* session,
    AudioLiveCaptureEncodeResult* result);

AudioLiveCaptureEncodeRunResult RunAudioLiveCaptureEncode(
    const AudioTrackPlan& plan,
    const AudioLiveCaptureEncodeOptions& options,
    AudioEncodeSession* session,
    AudioLiveCaptureSourceFactory source_factory,
    AudioLiveCaptureEncodeResult* result);

const wchar_t* AudioLiveCaptureEncodeStatusName(
    AudioLiveCaptureEncodeStatus status) noexcept;

}  // namespace olouie::audio
