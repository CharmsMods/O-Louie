#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "encode/H264PacketStore.h"
#include "encode/MfHardwareH264EncoderProbe.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoRecordingBootstrap.h"
#include "graphics/DisplayManager.h"
#include "graphics/GpuBgraToNv12.h"
#include "record/PacketStore.h"
#include "record/SessionManifest.h"

namespace olouie::encode {

enum class VideoRecordingMetadataStatus {
  Success,
  InvalidConfig,
  MissingRuntimeInfo,
  MissingH264Config,
};

struct VideoRecordingMetadataInputs {
  std::filesystem::path packet_store_session_dir;
  std::filesystem::path packet_file_path;
  record::TrackDefinition video_track;
  MfHardwareH264EncoderConfig requested_config;
  H264PacketStoreConfig h264;
  MfHardwareH264EncoderSessionInfo encoder_info;
  graphics::MonitorInfo monitor;
  graphics::GpuBgraToNv12Plan conversion_plan;
};

struct VideoRecordingMetadata {
  std::filesystem::path packet_store_session_dir;
  std::filesystem::path packet_file_path;
  record::TrackDefinition video_track;
  MfHardwareH264EncoderConfig requested_config;
  H264PacketStoreConfig h264;

  std::wstring encoder_name;
  std::wstring encoder_clsid;
  uint32_t encoder_enumeration_flags = 0;
  MfHardwareH264EncoderMediaTypePlan media_type;
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

struct VideoRecordingMetadataResult {
  VideoRecordingMetadataStatus status =
      VideoRecordingMetadataStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

VideoRecordingMetadataResult BuildVideoRecordingMetadata(
    const VideoRecordingMetadataInputs& inputs,
    VideoRecordingMetadata* metadata);

VideoRecordingMetadataResult BuildVideoRecordingMetadata(
    const VideoRecordingBootstrapSession& bootstrap,
    VideoRecordingMetadata* metadata);

record::SessionManifest BuildVideoRecordingSessionManifest(
    const VideoRecordingMetadata& metadata);

const wchar_t* VideoRecordingMetadataStatusName(
    VideoRecordingMetadataStatus status) noexcept;

}  // namespace olouie::encode
