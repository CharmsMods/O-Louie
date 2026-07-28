#pragma once

#include <chrono>
#include <string>

#include "audio/AudioCaptureEncodeBridge.h"
#include "audio/AudioCaptureSmoke.h"
#include "audio/AudioTrackPlan.h"

namespace olouie::audio {

enum class AudioCaptureEncodeSmokeStatus {
  Success,
  InvalidConfig,
  CaptureFailed,
  DrainFailed,
};

struct AudioCaptureEncodeSmokeResult {
  bool bridge_configured = false;
  AudioCaptureSmokeResult capture;
  AudioEncodeSessionResult drain;
  CapturedPcmSessionSinkStats sink_stats;
  AudioSourceSessionDispatchResult last_dispatch;
};

struct AudioCaptureEncodeSmokeRunResult {
  AudioCaptureEncodeSmokeStatus status =
      AudioCaptureEncodeSmokeStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

AudioCaptureEncodeSmokeRunResult RunAudioCaptureEncodeSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    AudioEncodeSession* session,
    AudioCaptureEncodeSmokeResult* result);

AudioCaptureEncodeSmokeRunResult RunAudioCaptureEncodeSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    AudioEncodeSession* session,
    int64_t qpc_origin_ns,
    const AudioCaptureSmokeRunners& runners,
    AudioCaptureEncodeSmokeResult* result);

}  // namespace olouie::audio
