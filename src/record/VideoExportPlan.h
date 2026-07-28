#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "record/MuxPlan.h"
#include "record/PacketStore.h"
#include "record/SessionManifest.h"

namespace olouie::record {

enum class VideoExportPlanStatus {
  Success,
  InvalidRequest,
  InvalidManifest,
  ManifestReadFailed,
  PacketStoreRecoverFailed,
  PacketStoreSnapshotFailed,
  MetadataMismatch,
  NoPackets,
  MissingAudioPackets,
  NoKeyframe,
  MuxPlanFailed,
};

struct VideoExportTrackMetadata {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  std::wstring h264_packet_format;
  std::vector<uint8_t> h264_sps;
  std::vector<uint8_t> h264_pps;
  std::vector<uint8_t> h264_avcc_extradata;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_numerator = 0;
  uint32_t fps_denominator = 0;

  bool IsReady() const noexcept;
};

struct AudioExportTrackMetadata {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  std::wstring source_kind;
  uint32_t source_index = 0;
  std::wstring name;
  uint32_t sample_rate = 0;
  uint16_t channel_count = 0;
  uint32_t bitrate_bps = 0;
  uint32_t aac_frame_samples = 0;
  uint32_t aac_payload_type = 0;
  uint32_t aac_profile_level_indication = 0;
  uint32_t aac_audio_object_type = 0;
  std::vector<uint8_t> aac_audio_specific_config;
  std::wstring encoder_name;

  bool IsReady() const noexcept;
};

struct VideoExportPlan {
  std::filesystem::path session_dir;
  std::filesystem::path packet_file_path;
  VideoExportTrackMetadata video;
  std::vector<AudioExportTrackMetadata> audio_tracks;
  std::vector<uint32_t> omitted_audio_track_ids;
  MuxPlan mux_plan;

  bool IsReady() const noexcept;
};

struct VideoExportPlanOptions {
  int64_t requested_start_ns = 0;
  int64_t requested_end_ns = 0;
  bool include_previous_keyframe = true;
  bool require_keyframe_start = true;
  bool normalize_timestamps = true;
  bool require_all_audio_tracks = false;
};

struct VideoExportPlanResult {
  VideoExportPlanStatus status = VideoExportPlanStatus::InvalidRequest;
  std::wstring message;

  bool Succeeded() const noexcept;
};

VideoExportPlanResult BuildVideoExportPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan);

VideoExportPlanResult BuildVideoExportPlan(
    const SessionManifest& manifest,
    const PacketStoreExportSnapshot& snapshot,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan);

VideoExportPlanResult BuildRecoveredVideoExportPlan(
    const std::filesystem::path& session_dir,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan);

const wchar_t* VideoExportPlanStatusName(
    VideoExportPlanStatus status) noexcept;

}  // namespace olouie::record
