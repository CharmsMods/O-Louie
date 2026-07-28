#include "audio/AudioSource.h"

namespace olouie::audio {

bool IsCapturedAudioSourceKind(AudioTrackKind kind) noexcept {
  switch (kind) {
    case AudioTrackKind::SystemLoopback:
    case AudioTrackKind::Microphone:
    case AudioTrackKind::ProcessLoopback:
      return true;
    case AudioTrackKind::DefaultMixed:
      return false;
  }

  return false;
}

bool IsCapturedAudioSourceValid(CapturedAudioSource source) noexcept {
  switch (source.kind) {
    case AudioTrackKind::SystemLoopback:
    case AudioTrackKind::Microphone:
      return source.source_index == 0;
    case AudioTrackKind::ProcessLoopback:
      return true;
    case AudioTrackKind::DefaultMixed:
      return false;
  }

  return false;
}

bool SameCapturedAudioSource(CapturedAudioSource left,
                             CapturedAudioSource right) noexcept {
  return left.kind == right.kind && left.source_index == right.source_index;
}

}  // namespace olouie::audio
