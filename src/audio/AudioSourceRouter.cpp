#include "audio/AudioSourceRouter.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioSourceRouteResult Result(AudioSourceRouteStatus status,
                              std::wstring message,
                              uint32_t track_id = 0) {
  AudioSourceRouteResult result;
  result.status = status;
  result.message = std::move(message);
  result.track_id = track_id;
  return result;
}

bool HasDuplicateTrackId(const AudioTrackPlan& plan, uint32_t track_id,
                         size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (plan.tracks[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

bool HasDuplicateSourceRoute(const std::vector<AudioSourceRoute>& routes,
                             CapturedAudioSource source) noexcept {
  for (const auto& route : routes) {
    if (SameCapturedAudioSource(route.source, source)) {
      return true;
    }
  }
  return false;
}

bool PacketTracksMatchPlan(const AudioTrackPlan& plan) noexcept {
  if (plan.packet_tracks.size() != plan.tracks.size()) {
    return false;
  }

  for (size_t index = 0; index < plan.tracks.size(); ++index) {
    if (plan.packet_tracks[index].track_id != plan.tracks[index].track_id ||
        plan.packet_tracks[index].codec_id != record::CodecId::Aac) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool AudioSourceRouteResult::Succeeded() const noexcept {
  return status == AudioSourceRouteStatus::Success;
}

AudioSourceRouter::AudioSourceRouter(const AudioTrackPlan& plan) {
  if (!plan.HasTracks() || !PacketTracksMatchPlan(plan)) {
    return;
  }

  routes_.reserve(plan.tracks.size());
  for (size_t index = 0; index < plan.tracks.size(); ++index) {
    const auto& track = plan.tracks[index];
    if (track.track_id == 0 ||
        HasDuplicateTrackId(plan, track.track_id, index)) {
      routes_.clear();
      return;
    }

    if (!IsCapturedAudioSourceKind(track.kind)) {
      continue;
    }

    CapturedAudioSource source{track.kind, track.source_index};
    if (!IsCapturedAudioSourceValid(source) ||
        HasDuplicateSourceRoute(routes_, source)) {
      routes_.clear();
      return;
    }

    routes_.push_back(AudioSourceRoute{source, track.track_id});
  }

  configured_ = true;
}

bool AudioSourceRouter::IsConfigured() const noexcept {
  return configured_;
}

size_t AudioSourceRouter::route_count() const noexcept {
  return routes_.size();
}

AudioSourceRouteResult AudioSourceRouter::ResolveTrack(
    CapturedAudioSource source) const {
  if (!configured_) {
    return Result(AudioSourceRouteStatus::InvalidPlan,
                  L"Audio source router is not configured.");
  }

  if (!IsCapturedAudioSourceValid(source)) {
    return Result(AudioSourceRouteStatus::InvalidSource,
                  L"Audio source routing needs a valid captured source.");
  }

  for (const auto& route : routes_) {
    if (SameCapturedAudioSource(route.source, source)) {
      return Result(AudioSourceRouteStatus::Success, L"", route.track_id);
    }
  }

  return Result(AudioSourceRouteStatus::SourceNotEnabled,
                L"Audio source routing has no enabled track for the source.");
}

}  // namespace olouie::audio
