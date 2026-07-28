#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/AacEncoder.h"
#include "audio/AudioEncodeSessionBinding.h"
#include "audio/AudioSource.h"
#include "audio/AudioTrackPlan.h"
#include "audio/PreparedPcmQueue.h"
#include "record/PacketStore.h"

namespace olouie::audio {

enum class AudioCaptureEncodeSetupStatus {
  Success,
  InvalidConfig,
  SourceUnavailable,
  UnsupportedFormat,
  TrackPlanFailed,
  EncoderInitFailed,
  BindingFailed,
  SessionCreateFailed,
};

struct AudioCaptureEncodeSetupResult {
  AudioCaptureEncodeSetupStatus status =
      AudioCaptureEncodeSetupStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct AudioCaptureEncodePreflightOptions {
  uint32_t first_track_id = 2;
  bool system_loopback = true;
  bool microphone = false;
  bool require_system_loopback = true;
  bool require_microphone = false;
  bool separate_source_tracks = true;
  bool default_mixed_track = false;
  uint32_t output_sample_rate = 0;
};

struct AudioCaptureEncodeSourcePreflight {
  CapturedAudioSource source;
  bool requested = false;
  bool required = false;
  bool included = false;
  PcmStreamFormat captured_format;
  std::wstring message;
};

struct AudioCaptureEncodeTrackPreflight {
  uint32_t track_id = 0;
  AudioTrackKind kind = AudioTrackKind::SystemLoopback;
  uint32_t source_index = 0;
  PcmStreamFormat captured_format;
  PcmStreamFormat prepared_format;
};

struct AudioCaptureEncodePreflight {
  AudioTrackPlan plan;
  std::vector<AudioCaptureEncodeSourcePreflight> sources;
  std::vector<AudioEncodeSessionFormatSlot> format_slots;
  std::vector<AudioCaptureEncodeTrackPreflight> tracks;

  void Reset();
  bool IsUsable() const noexcept;
};

struct AudioCaptureEncodeSessionSetupOptions {
  size_t queue_capacity = 0;
  PreparedPcmOverflowPolicy overflow_policy =
      PreparedPcmOverflowPolicy::RejectNewest;
  uint32_t aac_bitrate_bps = 192000;
};

struct AudioCaptureEncodeEncoderInfo {
  uint32_t track_id = 0;
  std::wstring backend_name;
  PcmStreamFormat prepared_format;
  AacEncoderOutputMetadata output_metadata;
};

struct AudioCaptureEncodeSessionSetup {
  std::vector<AudioCaptureEncodeEncoderInfo> encoder_infos;
  std::vector<std::unique_ptr<IAacEncoder>> encoders;
  std::vector<AudioEncodeSessionEncoderSlot> encoder_slots;
  std::vector<AudioEncodeSessionTrack> session_tracks;
  std::unique_ptr<AudioEncodeSession> session;

  void Reset();
  bool IsConfigured() const noexcept;
};

using AudioCaptureEncodeFormatProvider =
    bool (*)(CapturedAudioSource source,
             PcmStreamFormat* format,
             std::wstring* error);

using AudioCaptureEncodeAacEncoderFactory =
    AacEncoderResult (*)(const AudioTrack& track,
                         const PcmStreamFormat& prepared_format,
                         uint32_t bitrate_bps,
                         std::unique_ptr<IAacEncoder>* encoder,
                         std::wstring* backend_name,
                         AacEncoderOutputMetadata* output_metadata);

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodePreflight(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodePreflight* preflight);

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodePreflight(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodeFormatProvider format_provider,
    AudioCaptureEncodePreflight* preflight);

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodeSessionSetup(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetupOptions& options,
    record::PacketStore* packet_store,
    AudioCaptureEncodeSessionSetup* setup);

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodeSessionSetup(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetupOptions& options,
    record::PacketStore* packet_store,
    AudioCaptureEncodeAacEncoderFactory encoder_factory,
    AudioCaptureEncodeSessionSetup* setup);

const wchar_t* AudioCaptureEncodeSetupStatusName(
    AudioCaptureEncodeSetupStatus status) noexcept;

}  // namespace olouie::audio
