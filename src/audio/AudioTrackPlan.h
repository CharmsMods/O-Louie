#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "record/PacketStore.h"

namespace olouie::audio {

enum class AudioTrackKind {
  DefaultMixed,
  SystemLoopback,
  Microphone,
  ProcessLoopback,
};

struct AudioTrackPlanOptions {
  uint32_t first_track_id = 2;
  bool system_loopback = true;
  bool microphone = false;
  uint32_t process_loopback_count = 0;
  bool separate_source_tracks = true;
  bool default_mixed_track = true;
};

struct AudioTrack {
  uint32_t track_id = 0;
  AudioTrackKind kind = AudioTrackKind::SystemLoopback;
  uint32_t source_index = 0;
  std::wstring name;

  record::TrackDefinition ToPacketTrack() const noexcept;
};

struct AudioTrackPlan {
  std::vector<AudioTrack> tracks;
  std::vector<record::TrackDefinition> packet_tracks;

  bool HasTracks() const noexcept;
  bool HasDefaultMixedTrack() const noexcept;
};

const wchar_t* AudioTrackKindName(AudioTrackKind kind) noexcept;

bool BuildAudioTrackPlan(const AudioTrackPlanOptions& options,
                         AudioTrackPlan* plan, std::wstring* error);

}  // namespace olouie::audio
