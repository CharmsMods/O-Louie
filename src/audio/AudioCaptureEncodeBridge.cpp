#include "audio/AudioCaptureEncodeBridge.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioEncodeSessionResult Result(AudioEncodeSessionStatus status,
                                std::wstring message) {
  AudioEncodeSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

}  // namespace

AudioCaptureEncodeBridge::AudioCaptureEncodeBridge(
    const AudioTrackPlan& plan,
    AudioEncodeSession* session,
    int64_t qpc_origin_ns)
    : router_(plan),
      session_(session),
      dispatcher_(&router_, session_),
      sink_(&dispatcher_, qpc_origin_ns) {}

bool AudioCaptureEncodeBridge::IsConfigured() const noexcept {
  return session_ != nullptr && router_.IsConfigured() &&
         dispatcher_.IsConfigured() && sink_.IsConfigured();
}

ICapturedPcmSink* AudioCaptureEncodeBridge::captured_pcm_sink() noexcept {
  return &sink_;
}

CapturedPcmSessionSink& AudioCaptureEncodeBridge::session_sink() noexcept {
  return sink_;
}

const CapturedPcmSessionSink& AudioCaptureEncodeBridge::session_sink()
    const noexcept {
  return sink_;
}

AudioEncodeSessionResult AudioCaptureEncodeBridge::DrainQueuedBlocks(
    size_t max_total_blocks) {
  if (session_ == nullptr) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio capture encode bridge needs an encode session.");
  }

  return session_->DrainQueuedBlocks(max_total_blocks);
}

AudioEncodeSessionResult AudioCaptureEncodeBridge::DrainAllQueuedBlocks() {
  if (session_ == nullptr) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio capture encode bridge needs an encode session.");
  }

  return session_->DrainAllQueuedBlocks();
}

const CapturedPcmSessionSinkStats& AudioCaptureEncodeBridge::sink_stats()
    const noexcept {
  return sink_.stats();
}

const AudioSourceSessionDispatchResult&
AudioCaptureEncodeBridge::last_dispatch_result() const noexcept {
  return sink_.last_dispatch_result();
}

const AudioSourceRouter& AudioCaptureEncodeBridge::router() const noexcept {
  return router_;
}

}  // namespace olouie::audio
