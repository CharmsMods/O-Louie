#include "encode/VideoRecordingMetadata.h"

#include <utility>

namespace olouie::encode {
namespace {

VideoRecordingMetadataResult Result(VideoRecordingMetadataStatus status,
                                    std::wstring message) {
  VideoRecordingMetadataResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool VideoTrackIsValid(const record::TrackDefinition& track) noexcept {
  return track.track_id != 0 && track.codec_id == record::CodecId::H264;
}

}  // namespace

bool VideoRecordingMetadata::IsReady() const noexcept {
  return !packet_store_session_dir.empty() && VideoTrackIsValid(video_track) &&
         h264.IsReady() && media_type.IsValid() && source_width > 0 &&
         source_height > 0 && output_width > 0 && output_height > 0;
}

bool VideoRecordingMetadataResult::Succeeded() const noexcept {
  return status == VideoRecordingMetadataStatus::Success;
}

VideoRecordingMetadataResult BuildVideoRecordingMetadata(
    const VideoRecordingMetadataInputs& inputs,
    VideoRecordingMetadata* metadata) {
  if (metadata == nullptr) {
    return Result(VideoRecordingMetadataStatus::InvalidConfig,
                  L"Video recording metadata needs an output destination.");
  }

  *metadata = {};

  if (inputs.packet_store_session_dir.empty() ||
      !VideoTrackIsValid(inputs.video_track)) {
    return Result(VideoRecordingMetadataStatus::InvalidConfig,
                  L"Video recording metadata needs a PacketStore session path "
                  L"and H.264 video track.");
  }

  if (!inputs.h264.IsReady() ||
      inputs.h264.track_id != inputs.video_track.track_id) {
    return Result(VideoRecordingMetadataStatus::MissingH264Config,
                  L"Video recording metadata needs ready H.264 SPS/PPS/AVCC "
                  L"config for the video track.");
  }

  if (!inputs.encoder_info.media_type.IsValid() ||
      !inputs.conversion_plan.IsValid() || inputs.monitor.handle == nullptr ||
      inputs.monitor.Width() <= 0 || inputs.monitor.Height() <= 0) {
    return Result(VideoRecordingMetadataStatus::MissingRuntimeInfo,
                  L"Video recording metadata needs encoder media type, "
                  L"conversion plan, and monitor details.");
  }

  metadata->packet_store_session_dir = inputs.packet_store_session_dir;
  metadata->packet_file_path = inputs.packet_file_path;
  metadata->video_track = inputs.video_track;
  metadata->requested_config = inputs.requested_config;
  metadata->h264 = inputs.h264;

  metadata->encoder_name = inputs.encoder_info.encoder.name;
  metadata->encoder_clsid = inputs.encoder_info.encoder.clsid;
  metadata->encoder_enumeration_flags =
      inputs.encoder_info.encoder.enumeration_flags;
  metadata->media_type = inputs.encoder_info.media_type;
  metadata->d3d11_aware = inputs.encoder_info.d3d11_aware;
  metadata->device_manager_attached =
      inputs.encoder_info.device_manager_attached;
  metadata->async_transform = inputs.encoder_info.async_transform;
  metadata->async_unlocked = inputs.encoder_info.async_unlocked;
  metadata->codec_api_available = inputs.encoder_info.codec_api_available;

  metadata->monitor_device_name = inputs.monitor.device_name;
  metadata->monitor_primary = inputs.monitor.primary;
  metadata->monitor_left = inputs.monitor.monitor_rect.left;
  metadata->monitor_top = inputs.monitor.monitor_rect.top;
  metadata->monitor_right = inputs.monitor.monitor_rect.right;
  metadata->monitor_bottom = inputs.monitor.monitor_rect.bottom;
  metadata->source_width = inputs.conversion_plan.source_width;
  metadata->source_height = inputs.conversion_plan.source_height;
  metadata->output_width = inputs.conversion_plan.output_width;
  metadata->output_height = inputs.conversion_plan.output_height;

  return Result(VideoRecordingMetadataStatus::Success, L"");
}

VideoRecordingMetadataResult BuildVideoRecordingMetadata(
    const VideoRecordingBootstrapSession& bootstrap,
    VideoRecordingMetadata* metadata) {
  if (metadata == nullptr) {
    return Result(VideoRecordingMetadataStatus::InvalidConfig,
                  L"Video recording metadata needs an output destination.");
  }

  *metadata = {};

  if (!bootstrap.IsPrepared() || bootstrap.encoder_session == nullptr ||
      bootstrap.recording_session == nullptr ||
      bootstrap.recording_session->encode_chain() == nullptr) {
    return Result(VideoRecordingMetadataStatus::MissingRuntimeInfo,
                  L"Video recording metadata needs a prepared video "
                  L"bootstrap session.");
  }

  const auto* recording = bootstrap.recording_session.get();
  const auto* chain = recording->encode_chain();

  VideoRecordingMetadataInputs inputs;
  inputs.packet_store_session_dir = bootstrap.packet_store.session_dir();
  inputs.packet_file_path = bootstrap.packet_store.packet_file_path();
  inputs.video_track = recording->preflight().video_track;
  inputs.requested_config = recording->preflight().encoder_config;
  inputs.h264 = chain->session_config();
  inputs.encoder_info = bootstrap.encoder_session->info();
  inputs.monitor = bootstrap.monitor;
  inputs.conversion_plan = chain->conversion_plan();
  return BuildVideoRecordingMetadata(inputs, metadata);
}

record::SessionManifest BuildVideoRecordingSessionManifest(
    const VideoRecordingMetadata& metadata) {
  record::SessionManifest manifest;
  if (!metadata.IsReady()) {
    return manifest;
  }

  manifest.session_dir = metadata.packet_store_session_dir;
  manifest.packet_file_path = metadata.packet_file_path;

  auto& video = manifest.video;
  video.track_id = metadata.video_track.track_id;
  video.codec_id = metadata.video_track.codec_id;
  video.h264_packet_format =
      metadata.h264.packet_format == MfHardwareH264PacketFormat::AnnexB
          ? L"annex_b"
          : L"unknown";
  video.h264_sps = metadata.h264.sps;
  video.h264_pps = metadata.h264.pps;
  video.h264_avcc_extradata = metadata.h264.avcc_extradata;

  video.requested_width = metadata.requested_config.width;
  video.requested_height = metadata.requested_config.height;
  video.requested_fps_numerator =
      metadata.requested_config.fps_numerator;
  video.requested_fps_denominator =
      metadata.requested_config.fps_denominator;
  video.requested_bitrate_bps = metadata.requested_config.bitrate_bps;
  video.requested_gop_seconds = metadata.requested_config.gop_seconds;
  video.requested_max_b_frames = metadata.requested_config.max_b_frames;

  video.encoder_name = metadata.encoder_name;
  video.encoder_clsid = metadata.encoder_clsid;
  video.encoder_enumeration_flags = metadata.encoder_enumeration_flags;
  video.media_width = metadata.media_type.width;
  video.media_height = metadata.media_type.height;
  video.media_fps_numerator = metadata.media_type.fps_numerator;
  video.media_fps_denominator = metadata.media_type.fps_denominator;
  video.media_bitrate_bps = metadata.media_type.bitrate_bps;
  video.media_gop_frame_count = metadata.media_type.gop_frame_count;
  video.media_h264_profile = metadata.media_type.h264_profile;
  video.media_max_b_frames = metadata.media_type.max_b_frames;
  video.d3d11_aware = metadata.d3d11_aware;
  video.device_manager_attached = metadata.device_manager_attached;
  video.async_transform = metadata.async_transform;
  video.async_unlocked = metadata.async_unlocked;
  video.codec_api_available = metadata.codec_api_available;

  video.monitor_device_name = metadata.monitor_device_name;
  video.monitor_primary = metadata.monitor_primary;
  video.monitor_left = metadata.monitor_left;
  video.monitor_top = metadata.monitor_top;
  video.monitor_right = metadata.monitor_right;
  video.monitor_bottom = metadata.monitor_bottom;
  video.source_width = metadata.source_width;
  video.source_height = metadata.source_height;
  video.output_width = metadata.output_width;
  video.output_height = metadata.output_height;
  return manifest;
}

const wchar_t* VideoRecordingMetadataStatusName(
    VideoRecordingMetadataStatus status) noexcept {
  switch (status) {
    case VideoRecordingMetadataStatus::Success:
      return L"success";
    case VideoRecordingMetadataStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecordingMetadataStatus::MissingRuntimeInfo:
      return L"missing runtime info";
    case VideoRecordingMetadataStatus::MissingH264Config:
      return L"missing H.264 config";
  }

  return L"unknown";
}

}  // namespace olouie::encode
