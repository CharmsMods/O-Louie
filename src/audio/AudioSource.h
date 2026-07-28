#pragma once

#include <cstdint>

#include "audio/AudioTrackPlan.h"

namespace olouie::audio {

struct CapturedAudioSource {
  AudioTrackKind kind = AudioTrackKind::SystemLoopback;
  uint32_t source_index = 0;
};

bool IsCapturedAudioSourceKind(AudioTrackKind kind) noexcept;
bool IsCapturedAudioSourceValid(CapturedAudioSource source) noexcept;
bool SameCapturedAudioSource(CapturedAudioSource left,
                             CapturedAudioSource right) noexcept;

}  // namespace olouie::audio
