#include "audio/AudioCaptureEncodeSmoke.h"

#include <chrono>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioCaptureEncodeSmokeRunResult Result(
    AudioCaptureEncodeSmokeStatus status,
    std::wstring message) {
  AudioCaptureEncodeSmokeRunResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

AudioCaptureEncodeSmokeStatus StatusFromCapture(
    AudioCaptureSmokeStatus status) noexcept {
  switch (status) {
    case AudioCaptureSmokeStatus::Success:
      return AudioCaptureEncodeSmokeStatus::Success;
    case AudioCaptureSmokeStatus::InvalidConfig:
      return AudioCaptureEncodeSmokeStatus::InvalidConfig;
    case AudioCaptureSmokeStatus::SourceFailed:
      return AudioCaptureEncodeSmokeStatus::CaptureFailed;
  }

  return AudioCaptureEncodeSmokeStatus::CaptureFailed;
}

}  // namespace

bool AudioCaptureEncodeSmokeRunResult::Succeeded() const noexcept {
  return status == AudioCaptureEncodeSmokeStatus::Success;
}

AudioCaptureEncodeSmokeRunResult RunAudioCaptureEncodeSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    AudioEncodeSession* session,
    AudioCaptureEncodeSmokeResult* result) {
  return RunAudioCaptureEncodeSmoke(plan, duration, session, 0,
                                    DefaultAudioCaptureSmokeRunners(), result);
}

AudioCaptureEncodeSmokeRunResult RunAudioCaptureEncodeSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    AudioEncodeSession* session,
    int64_t qpc_origin_ns,
    const AudioCaptureSmokeRunners& runners,
    AudioCaptureEncodeSmokeResult* result) {
  if (result == nullptr) {
    return Result(AudioCaptureEncodeSmokeStatus::InvalidConfig,
                  L"Audio capture encode smoke needs an output destination.");
  }

  *result = {};

  AudioCaptureEncodeBridge bridge(plan, session, qpc_origin_ns);
  result->bridge_configured = bridge.IsConfigured();
  if (!bridge.IsConfigured()) {
    return Result(AudioCaptureEncodeSmokeStatus::InvalidConfig,
                  L"Audio capture encode smoke needs a configured bridge.");
  }

  const auto captured = RunAudioCaptureSmoke(
      plan, duration, bridge.captured_pcm_sink(), runners, &result->capture);

  result->drain = bridge.DrainAllQueuedBlocks();
  result->sink_stats = bridge.sink_stats();
  result->last_dispatch = bridge.last_dispatch_result();

  if (!result->drain.Succeeded()) {
    return Result(AudioCaptureEncodeSmokeStatus::DrainFailed,
                  result->drain.message);
  }

  if (!captured.Succeeded()) {
    return Result(StatusFromCapture(captured.status), captured.message);
  }

  return Result(AudioCaptureEncodeSmokeStatus::Success, L"");
}

}  // namespace olouie::audio
