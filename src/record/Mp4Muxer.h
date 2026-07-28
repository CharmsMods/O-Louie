#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "record/DiskWriteFault.h"
#include "record/MuxPlan.h"
#include "record/VideoExportPlan.h"

namespace olouie::record {

enum class Mp4MuxStatus {
  Success,
  InvalidRequest,
  BackendUnavailable,
  DestinationExists,
  FileSystemError,
};

enum class Mp4MuxBackendStatus {
  NotConfigured,
  Configured,
};

struct Mp4MuxBackendAvailability {
  Mp4MuxBackendStatus status = Mp4MuxBackendStatus::NotConfigured;
  bool dynamic_linking_expected = true;
  std::vector<std::wstring> required_libraries;
  std::wstring message;

  bool Available() const noexcept;
};

struct Mp4H264VideoTrack {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_numerator = 0;
  uint32_t fps_denominator = 0;
  std::wstring packet_format;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  std::vector<uint8_t> avcc_extradata;

  bool IsReady() const noexcept;
};

struct Mp4AacAudioTrack {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  std::wstring source_kind;
  std::wstring name;
  uint32_t sample_rate = 0;
  uint16_t channel_count = 0;
  uint32_t bitrate_bps = 0;
  uint32_t frame_samples = 0;
  uint32_t payload_type = 0;
  uint32_t profile_level_indication = 0;
  uint32_t audio_object_type = 0;
  std::vector<uint8_t> audio_specific_config;
  std::wstring encoder_name;

  bool IsReady() const noexcept;
};

struct Mp4MuxRequest {
  std::filesystem::path temp_output_path;
  std::filesystem::path final_output_path;
  std::filesystem::path packet_file_path;
  Mp4H264VideoTrack video_track;
  std::vector<Mp4AacAudioTrack> audio_tracks;
  MuxPlan plan;
  bool allow_overwrite = false;
};

struct Mp4MuxResult {
  Mp4MuxStatus status = Mp4MuxStatus::InvalidRequest;
  std::wstring message;
  DiskWriteFault write_fault;

  bool Succeeded() const noexcept;
};

struct Mp4MuxPayloadReadStats {
  uint64_t packet_count = 0;
  uint64_t video_packet_count = 0;
  uint64_t audio_packet_count = 0;
  uint64_t payload_byte_count = 0;
  uint64_t video_payload_byte_count = 0;
  uint64_t audio_payload_byte_count = 0;
  int64_t first_video_dts_ns = 0;
  int64_t last_video_dts_ns = 0;

  bool HasVideoPackets() const noexcept;
};

struct Mp4MuxStreamSetupStats {
  bool output_context_allocated = false;
  bool video_stream_created = false;
  bool h264_parameters_applied = false;
  uint32_t audio_stream_count = 0;
  uint32_t aac_parameters_applied_count = 0;
  uint32_t video_track_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_numerator = 0;
  uint32_t fps_denominator = 0;
  uint64_t extradata_bytes = 0;
  uint64_t audio_extradata_bytes = 0;

  bool IsReady() const noexcept;
};

Mp4MuxResult BuildVideoMp4MuxRequest(
    const VideoExportPlan& export_plan,
    const std::filesystem::path& temp_output_path,
    const std::filesystem::path& final_output_path,
    bool allow_overwrite,
    Mp4MuxRequest* request);

const wchar_t* Mp4MuxStatusName(Mp4MuxStatus status) noexcept;
const wchar_t* Mp4MuxBackendStatusName(
    Mp4MuxBackendStatus status) noexcept;

class Mp4Muxer final {
 public:
  Mp4MuxResult WriteMp4(const Mp4MuxRequest& request) const;

  static Mp4MuxBackendAvailability BackendAvailability();
  static Mp4MuxResult DryRunPayloadRead(
      const Mp4MuxRequest& request,
      Mp4MuxPayloadReadStats* stats);
  static Mp4MuxResult ValidateVideoStreamSetup(
      const Mp4MuxRequest& request,
      Mp4MuxStreamSetupStats* stats);
  static Mp4MuxResult ValidateStreamSetup(
      const Mp4MuxRequest& request,
      Mp4MuxStreamSetupStats* stats);
  static Mp4MuxResult ValidateRequest(const Mp4MuxRequest& request);
  static Mp4MuxResult AtomicRename(const std::filesystem::path& temp_path,
                                   const std::filesystem::path& final_path,
                                   bool allow_overwrite);
};

}  // namespace olouie::record
