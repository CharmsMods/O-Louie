#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "record/Bookmark.h"
#include "record/DiskWriteFault.h"
#include "record/PacketStore.h"

namespace olouie::record {

constexpr uint32_t kSessionManifestVersion = 3;
constexpr uint32_t kLegacyVideoOnlySessionManifestVersion = 1;
constexpr uint32_t kAudioSessionManifestVersion = 2;

enum class SessionManifestStatus {
  Success,
  InvalidConfig,
  WriteFailed,
  ReadFailed,
  ParseFailed,
  UnsupportedVersion,
};

struct VideoTrackSessionManifest {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  std::wstring h264_packet_format;
  std::vector<uint8_t> h264_sps;
  std::vector<uint8_t> h264_pps;
  std::vector<uint8_t> h264_avcc_extradata;

  uint32_t requested_width = 0;
  uint32_t requested_height = 0;
  uint32_t requested_fps_numerator = 0;
  uint32_t requested_fps_denominator = 0;
  uint32_t requested_bitrate_bps = 0;
  double requested_gop_seconds = 0.0;
  uint32_t requested_max_b_frames = 0;

  std::wstring encoder_name;
  std::wstring encoder_clsid;
  uint32_t encoder_enumeration_flags = 0;
  uint32_t media_width = 0;
  uint32_t media_height = 0;
  uint32_t media_fps_numerator = 0;
  uint32_t media_fps_denominator = 0;
  uint32_t media_bitrate_bps = 0;
  uint32_t media_gop_frame_count = 0;
  uint32_t media_h264_profile = 0;
  uint32_t media_max_b_frames = 0;
  bool d3d11_aware = false;
  bool device_manager_attached = false;
  bool async_transform = false;
  bool async_unlocked = false;
  bool codec_api_available = false;

  std::wstring monitor_device_name;
  bool monitor_primary = false;
  int32_t monitor_left = 0;
  int32_t monitor_top = 0;
  int32_t monitor_right = 0;
  int32_t monitor_bottom = 0;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;

  bool IsReady() const noexcept;
};

struct AudioTrackSessionManifest {
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

struct SessionManifest {
  uint32_t version = kSessionManifestVersion;
  std::filesystem::path session_dir;
  std::filesystem::path packet_file_path;
  VideoTrackSessionManifest video;
  std::vector<AudioTrackSessionManifest> audio_tracks;
  std::vector<Bookmark> bookmarks;

  bool IsReady() const noexcept;
};

struct SessionManifestResult {
  SessionManifestStatus status = SessionManifestStatus::InvalidConfig;
  std::wstring message;
  DiskWriteFault write_fault;

  bool Succeeded() const noexcept;
};

std::filesystem::path SessionManifestPath(
    const std::filesystem::path& session_dir);

SessionManifestResult WriteSessionManifest(
    const SessionManifest& manifest);

SessionManifestResult ReadSessionManifest(
    const std::filesystem::path& session_dir,
    SessionManifest* manifest);

SessionManifestResult ReadSessionManifestFile(
    const std::filesystem::path& manifest_file_path,
    SessionManifest* manifest);

const wchar_t* SessionManifestStatusName(
    SessionManifestStatus status) noexcept;

}  // namespace olouie::record
