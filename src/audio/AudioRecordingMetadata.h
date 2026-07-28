#pragma once

#include <string>
#include <vector>

#include "audio/AudioCaptureEncodeSetup.h"
#include "record/SessionManifest.h"

namespace olouie::audio {

enum class AudioRecordingMetadataStatus {
  Success,
  InvalidConfig,
  MetadataMismatch,
};

struct AudioRecordingMetadataResult {
  AudioRecordingMetadataStatus status =
      AudioRecordingMetadataStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

AudioRecordingMetadataResult BuildAudioRecordingMetadata(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetup& setup,
    std::vector<record::AudioTrackSessionManifest>* tracks);

const wchar_t* AudioRecordingMetadataStatusName(
    AudioRecordingMetadataStatus status) noexcept;

}  // namespace olouie::audio
