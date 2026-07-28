#include "audio/AudioSourceSessionDispatcher.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioSourceSessionDispatchResult Result(
    AudioSourceSessionDispatchStatus status,
    std::wstring message,
    AudioSourceRouteStatus route_status,
    AudioEncodeSessionStatus queue_status,
    uint32_t track_id = 0) {
  AudioSourceSessionDispatchResult result;
  result.status = status;
  result.message = std::move(message);
  result.route_status = route_status;
  result.queue_status = queue_status;
  result.track_id = track_id;
  return result;
}

}  // namespace

bool AudioSourceSessionDispatchResult::Succeeded() const noexcept {
  return status == AudioSourceSessionDispatchStatus::Success;
}

AudioSourceSessionDispatcher::AudioSourceSessionDispatcher(
    const AudioSourceRouter* router,
    AudioEncodeSession* session)
    : router_(router), session_(session) {}

bool AudioSourceSessionDispatcher::IsConfigured() const noexcept {
  return router_ != nullptr && router_->IsConfigured() && session_ != nullptr &&
         session_->IsConfigured();
}

AudioSourceSessionDispatchResult
AudioSourceSessionDispatcher::QueueCapturedPcm(
    CapturedAudioSource source,
    const PcmStreamFormat& format,
    const PcmPacketInfo& packet,
    int64_t pts_ns,
    std::span<const std::byte> pcm_bytes) {
  if (router_ == nullptr || session_ == nullptr) {
    return Result(AudioSourceSessionDispatchStatus::InvalidConfig,
                  L"Audio source session dispatcher needs a router and "
                  L"session.",
                  AudioSourceRouteStatus::InvalidPlan,
                  AudioEncodeSessionStatus::InvalidConfig);
  }

  const auto routed = router_->ResolveTrack(source);
  if (!routed.Succeeded()) {
    return Result(AudioSourceSessionDispatchStatus::RouteError,
                  routed.message, routed.status,
                  AudioEncodeSessionStatus::InvalidConfig, routed.track_id);
  }

  const auto queued =
      session_->QueueCapturedPcm(routed.track_id, format, packet, pts_ns,
                                 pcm_bytes);
  if (!queued.Succeeded()) {
    return Result(AudioSourceSessionDispatchStatus::QueueError,
                  queued.message, AudioSourceRouteStatus::Success,
                  queued.status, queued.track_id);
  }

  return Result(AudioSourceSessionDispatchStatus::Success, L"",
                AudioSourceRouteStatus::Success,
                AudioEncodeSessionStatus::Success, queued.track_id);
}

}  // namespace olouie::audio
