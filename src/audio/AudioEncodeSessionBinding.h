#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/AacEncoder.h"
#include "audio/AudioEncodeSession.h"
#include "audio/AudioTrackPlan.h"
#include "audio/PcmAudio.h"
#include "audio/PreparedPcmQueue.h"

namespace olouie::audio {

struct AudioEncodeSessionBindingOptions {
  size_t queue_capacity = 0;
  PreparedPcmOverflowPolicy overflow_policy =
      PreparedPcmOverflowPolicy::RejectNewest;
};

struct AudioEncodeSessionFormatSlot {
  uint32_t track_id = 0;
  PcmStreamFormat input_format;
  uint32_t output_sample_rate = 0;
};

struct AudioEncodeSessionEncoderSlot {
  uint32_t track_id = 0;
  IAacEncoder* encoder = nullptr;
};

enum class AudioEncodeSessionBindingStatus {
  Success,
  InvalidPlan,
  InvalidOptions,
  MissingFormat,
  DuplicateFormat,
  UnexpectedFormat,
  MissingEncoder,
  DuplicateEncoder,
  UnexpectedEncoder,
};

struct AudioEncodeSessionBindingResult {
  AudioEncodeSessionBindingStatus status =
      AudioEncodeSessionBindingStatus::InvalidPlan;
  std::wstring message;

  bool Succeeded() const noexcept;
};

AudioEncodeSessionBindingResult BuildAudioEncodeSessionTracks(
    const AudioTrackPlan& plan,
    const AudioEncodeSessionBindingOptions& options,
    std::span<const AudioEncodeSessionFormatSlot> format_slots,
    std::span<const AudioEncodeSessionEncoderSlot> encoder_slots,
    std::vector<AudioEncodeSessionTrack>* tracks);

}  // namespace olouie::audio
