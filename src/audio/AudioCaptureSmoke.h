#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/AudioCaptureManager.h"
#include "audio/CapturedPcmSink.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

enum class AudioCaptureSmokeStatus {
  Success,
  InvalidConfig,
  SourceFailed,
};

struct AudioCaptureSourceSmokeResult {
  CapturedAudioSource source;
  AudioCaptureSourceRuntime runtime =
      AudioCaptureSourceRuntime::SystemLoopback;
  AudioCaptureSourceSupport support = AudioCaptureSourceSupport::Deferred;
  uint32_t track_id = 0;
  bool attempted = false;
  bool succeeded = false;
  PcmCaptureStats capture;
  std::wstring message;
};

struct AudioCaptureSmokeResult {
  bool deferred_mixed_track = false;
  size_t attempted_source_count = 0;
  size_t succeeded_source_count = 0;
  size_t deferred_source_count = 0;
  uint64_t packet_count = 0;
  uint64_t frame_count = 0;
  std::vector<AudioCaptureSourceSmokeResult> sources;
};

struct AudioCaptureSmokeRunResult {
  AudioCaptureSmokeStatus status = AudioCaptureSmokeStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

using AudioCaptureSmokeSourceRunner =
    bool (*)(std::chrono::milliseconds duration,
             PcmCaptureStats* result,
             ICapturedPcmSink* sink,
             std::wstring* error);

struct AudioCaptureSmokeRunners {
  AudioCaptureSmokeSourceRunner system_loopback = nullptr;
  AudioCaptureSmokeSourceRunner microphone = nullptr;
};

AudioCaptureSmokeRunners DefaultAudioCaptureSmokeRunners() noexcept;

AudioCaptureSmokeRunResult RunAudioCaptureSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    ICapturedPcmSink* sink,
    AudioCaptureSmokeResult* result);

AudioCaptureSmokeRunResult RunAudioCaptureSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    ICapturedPcmSink* sink,
    const AudioCaptureSmokeRunners& runners,
    AudioCaptureSmokeResult* result);

}  // namespace olouie::audio
