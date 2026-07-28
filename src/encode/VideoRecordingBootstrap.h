#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "encode/MfHardwareH264EncoderProbe.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoRecordingRunSession.h"
#include "graphics/D3D11DeviceContext.h"
#include "graphics/DisplayManager.h"
#include "record/PacketStore.h"

namespace olouie::encode {

enum class VideoRecordingBootstrapStatus {
  Success,
  InvalidConfig,
  MonitorUnavailable,
  D3DCreateFailed,
  EncoderInitFailed,
  PacketStoreCreateFailed,
  RecordingPreflightFailed,
  RecordingPrepareFailed,
};

struct VideoRecordingBootstrapOptions {
  HMONITOR monitor = nullptr;
  std::filesystem::path packet_store_session_dir;
  VideoRecordingPreflightOptions preflight;
  MfHardwareH264EncoderProbeOptions encoder_probe_options;
  std::vector<record::TrackDefinition> additional_packet_tracks;
  WgcVideoFrameCopyRunner capture_runner = nullptr;
};

struct VideoRecordingBootstrapResult {
  VideoRecordingBootstrapStatus status =
      VideoRecordingBootstrapStatus::InvalidConfig;
  std::wstring message;
  MfHardwareH264EncoderProbeResult encoder_config_result;
  MfHardwareH264EncoderSessionResult encoder_result;
  VideoRecordingRunSessionResult preflight_result;
  VideoRecordingRunSessionResult prepare_result;

  bool Succeeded() const noexcept;
};

struct VideoRecordingBootstrapSession {
  graphics::MonitorInfo monitor;
  graphics::D3D11DeviceContext d3d;
  std::unique_ptr<MfHardwareH264EncoderSession> encoder_session;
  record::PacketStore packet_store;
  std::unique_ptr<VideoRecordingRunSession> recording_session;

  void Reset();
  bool IsPrepared() const noexcept;
};

VideoRecordingBootstrapResult BuildVideoRecordingBootstrapSession(
    const VideoRecordingBootstrapOptions& options,
    VideoRecordingBootstrapSession* session);

const wchar_t* VideoRecordingBootstrapStatusName(
    VideoRecordingBootstrapStatus status) noexcept;

}  // namespace olouie::encode
