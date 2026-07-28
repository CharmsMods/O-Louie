#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/AudioSource.h"
#include "audio/AudioTrackPlan.h"

namespace olouie::audio {

struct AudioSourceRoute {
  CapturedAudioSource source;
  uint32_t track_id = 0;
};

enum class AudioSourceRouteStatus {
  Success,
  InvalidPlan,
  InvalidSource,
  SourceNotEnabled,
};

struct AudioSourceRouteResult {
  AudioSourceRouteStatus status = AudioSourceRouteStatus::InvalidPlan;
  std::wstring message;
  uint32_t track_id = 0;

  bool Succeeded() const noexcept;
};

class AudioSourceRouter final {
 public:
  explicit AudioSourceRouter(const AudioTrackPlan& plan);

  bool IsConfigured() const noexcept;
  size_t route_count() const noexcept;
  AudioSourceRouteResult ResolveTrack(CapturedAudioSource source) const;

 private:
  bool configured_ = false;
  std::vector<AudioSourceRoute> routes_;
};

}  // namespace olouie::audio
