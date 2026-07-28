#include "audio/AudioRecordingMetadata.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace olouie::audio {
namespace {

AudioRecordingMetadataResult Result(AudioRecordingMetadataStatus status,
                                    std::wstring message) {
  AudioRecordingMetadataResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

std::wstring_view SourceKindToken(AudioTrackKind kind) noexcept {
  switch (kind) {
    case AudioTrackKind::DefaultMixed:
      return L"default_mixed";
    case AudioTrackKind::SystemLoopback:
      return L"system_loopback";
    case AudioTrackKind::Microphone:
      return L"microphone";
    case AudioTrackKind::ProcessLoopback:
      return L"process_loopback";
  }
  return {};
}

const AudioCaptureEncodeEncoderInfo* FindEncoderInfo(
    const AudioCaptureEncodeSessionSetup& setup,
    uint32_t track_id) noexcept {
  const auto found = std::find_if(
      setup.encoder_infos.begin(), setup.encoder_infos.end(),
      [track_id](const AudioCaptureEncodeEncoderInfo& info) {
        return info.track_id == track_id;
      });
  return found == setup.encoder_infos.end() ? nullptr : &(*found);
}

}  // namespace

bool AudioRecordingMetadataResult::Succeeded() const noexcept {
  return status == AudioRecordingMetadataStatus::Success;
}

AudioRecordingMetadataResult BuildAudioRecordingMetadata(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetup& setup,
    std::vector<record::AudioTrackSessionManifest>* tracks) {
  if (tracks == nullptr) {
    return Result(AudioRecordingMetadataStatus::InvalidConfig,
                  L"Audio recording metadata needs an output destination.");
  }
  tracks->clear();

  if (!preflight.IsUsable() || !setup.IsConfigured() ||
      preflight.plan.tracks.size() != setup.encoder_infos.size()) {
    return Result(AudioRecordingMetadataStatus::InvalidConfig,
                  L"Audio recording metadata needs matching prepared audio "
                  L"preflight and encoder setup state.");
  }

  std::set<uint32_t> track_ids;
  std::vector<record::AudioTrackSessionManifest> built;
  built.reserve(preflight.plan.tracks.size());
  for (const auto& track : preflight.plan.tracks) {
    const auto* encoder_info = FindEncoderInfo(setup, track.track_id);
    const auto source_kind = SourceKindToken(track.kind);
    if (encoder_info == nullptr || source_kind.empty() ||
        !track_ids.insert(track.track_id).second ||
        !encoder_info->output_metadata.IsReady()) {
      return Result(AudioRecordingMetadataStatus::MetadataMismatch,
                    L"Audio track planning and AAC encoder metadata do not "
                    L"describe the same unique tracks.");
    }

    const auto& output = encoder_info->output_metadata;
    record::AudioTrackSessionManifest manifest_track;
    manifest_track.track_id = track.track_id;
    manifest_track.codec_id = record::CodecId::Aac;
    manifest_track.source_kind = source_kind;
    manifest_track.source_index = track.source_index;
    manifest_track.name = track.name;
    manifest_track.sample_rate = output.sample_rate;
    manifest_track.channel_count = output.channel_count;
    manifest_track.bitrate_bps = output.bitrate_bps;
    manifest_track.aac_frame_samples = output.frame_samples;
    manifest_track.aac_payload_type = output.payload_type;
    manifest_track.aac_profile_level_indication =
        output.profile_level_indication;
    manifest_track.aac_audio_object_type = output.audio_object_type;
    manifest_track.aac_audio_specific_config =
        output.audio_specific_config;
    manifest_track.encoder_name = encoder_info->backend_name;
    if (!manifest_track.IsReady()) {
      return Result(AudioRecordingMetadataStatus::MetadataMismatch,
                    L"AAC encoder metadata is incomplete for session "
                    L"persistence.");
    }
    built.push_back(std::move(manifest_track));
  }

  *tracks = std::move(built);
  return Result(AudioRecordingMetadataStatus::Success, L"");
}

const wchar_t* AudioRecordingMetadataStatusName(
    AudioRecordingMetadataStatus status) noexcept {
  switch (status) {
    case AudioRecordingMetadataStatus::Success:
      return L"success";
    case AudioRecordingMetadataStatus::InvalidConfig:
      return L"invalid config";
    case AudioRecordingMetadataStatus::MetadataMismatch:
      return L"metadata mismatch";
  }
  return L"unknown";
}

}  // namespace olouie::audio
