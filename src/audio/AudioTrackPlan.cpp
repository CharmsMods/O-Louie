#include "audio/AudioTrackPlan.h"

#include <cstddef>
#include <limits>
#include <string>

namespace olouie::audio {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

uint64_t EnabledSourceCount(const AudioTrackPlanOptions& options) noexcept {
  uint64_t count = 0;
  if (options.system_loopback) {
    ++count;
  }
  if (options.microphone) {
    ++count;
  }
  return count + options.process_loopback_count;
}

bool AddTrack(AudioTrackKind kind, uint32_t source_index, std::wstring name,
              AudioTrackPlan* plan, uint32_t* next_track_id,
              std::wstring* error) {
  if (*next_track_id == 0) {
    SetError(error, L"Audio track ids must be nonzero.");
    return false;
  }

  AudioTrack track;
  track.track_id = *next_track_id;
  track.kind = kind;
  track.source_index = source_index;
  track.name = std::move(name);

  plan->packet_tracks.push_back(track.ToPacketTrack());
  plan->tracks.push_back(std::move(track));
  if (*next_track_id < std::numeric_limits<uint32_t>::max()) {
    ++(*next_track_id);
  }
  return true;
}

}  // namespace

record::TrackDefinition AudioTrack::ToPacketTrack() const noexcept {
  return {track_id, record::CodecId::Aac};
}

bool AudioTrackPlan::HasTracks() const noexcept {
  return !tracks.empty();
}

bool AudioTrackPlan::HasDefaultMixedTrack() const noexcept {
  for (const auto& track : tracks) {
    if (track.kind == AudioTrackKind::DefaultMixed) {
      return true;
    }
  }
  return false;
}

const wchar_t* AudioTrackKindName(AudioTrackKind kind) noexcept {
  switch (kind) {
    case AudioTrackKind::DefaultMixed:
      return L"Default mixed audio";
    case AudioTrackKind::SystemLoopback:
      return L"System loopback";
    case AudioTrackKind::Microphone:
      return L"Microphone";
    case AudioTrackKind::ProcessLoopback:
      return L"Process loopback";
  }

  return L"Unknown audio track";
}

bool BuildAudioTrackPlan(const AudioTrackPlanOptions& options,
                         AudioTrackPlan* plan, std::wstring* error) {
  if (plan == nullptr) {
    SetError(error, L"Audio track planning needs an output destination.");
    return false;
  }

  plan->tracks.clear();
  plan->packet_tracks.clear();

  if (options.first_track_id == 0) {
    SetError(error, L"Audio track ids must start at a nonzero value.");
    return false;
  }

  const uint64_t source_count = EnabledSourceCount(options);
  if (source_count == 0) {
    SetError(error, L"At least one audio source must be enabled.");
    return false;
  }

  if (!options.separate_source_tracks && !options.default_mixed_track) {
    SetError(error,
             L"Audio planning needs separate tracks or a default mixed track.");
    return false;
  }

  const bool add_mixed_track =
      options.default_mixed_track &&
      (!options.separate_source_tracks || source_count > 1);
  const uint64_t planned_track_count =
      (add_mixed_track ? 1u : 0u) +
      (options.separate_source_tracks ? source_count : 0u);

  if (planned_track_count == 0) {
    SetError(error, L"Audio planning produced no packet tracks.");
    return false;
  }

  constexpr uint64_t kMaxTrackId =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
  const uint64_t last_track_id =
      static_cast<uint64_t>(options.first_track_id) + planned_track_count - 1u;
  if (last_track_id > kMaxTrackId) {
    SetError(error, L"Audio track ids would overflow uint32_t.");
    return false;
  }

  plan->tracks.reserve(static_cast<size_t>(planned_track_count));
  plan->packet_tracks.reserve(static_cast<size_t>(planned_track_count));

  uint32_t next_track_id = options.first_track_id;
  if (add_mixed_track &&
      !AddTrack(AudioTrackKind::DefaultMixed, 0, L"Default mixed audio", plan,
                &next_track_id, error)) {
    return false;
  }

  if (!options.separate_source_tracks) {
    return true;
  }

  if (options.system_loopback &&
      !AddTrack(AudioTrackKind::SystemLoopback, 0, L"System loopback", plan,
                &next_track_id, error)) {
    return false;
  }

  if (options.microphone &&
      !AddTrack(AudioTrackKind::Microphone, 0, L"Microphone", plan,
                &next_track_id, error)) {
    return false;
  }

  for (uint32_t index = 0; index < options.process_loopback_count; ++index) {
    if (!AddTrack(AudioTrackKind::ProcessLoopback, index,
                  L"Process loopback " + std::to_wstring(index + 1), plan,
                  &next_track_id, error)) {
      return false;
    }
  }

  return true;
}

}  // namespace olouie::audio
