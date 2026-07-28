#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <d3d11.h>

#include "capture/VideoFrameQueue.h"
#include "encode/MfHardwareH264EncoderProbe.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoEncodeChain.h"
#include "encode/VideoLiveCaptureEncode.h"
#include "record/PacketStore.h"

namespace olouie::encode {

enum class VideoRecordingSetupStatus {
  Success,
  InvalidConfig,
  EncoderConfigInvalid,
  SessionCreateFailed,
};

struct VideoRecordingSetupResult {
  VideoRecordingSetupStatus status = VideoRecordingSetupStatus::InvalidConfig;
  std::wstring message;
  MfHardwareH264EncoderProbeResult encoder_config_result;
  BgraVideoRecordingSessionResult session_prepare_result;

  bool Succeeded() const noexcept;
};

struct VideoRecordingPreflightOptions {
  uint32_t video_track_id = 1;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  MfHardwareH264EncoderConfig encoder_config;
  uint32_t queue_capacity = 0;
  capture::VideoFrameOverflowPolicy overflow_policy =
      capture::VideoFrameOverflowPolicy::KeepNewest;
  size_t drain_frame_budget = 0;
  uint32_t session_drain_timeout_ms = 3000;
  VideoLiveCaptureEncodeOptions live;
};

struct VideoRecordingPreflight {
  record::TrackDefinition video_track;
  MfHardwareH264EncoderConfig encoder_config;
  VideoEncodeChainConfig chain_config;
  VideoLiveCaptureEncodeOptions live_options;

  void Reset();
  bool IsUsable() const noexcept;
};

struct VideoRecordingSessionSetup {
  std::unique_ptr<VideoEncodeChain> chain;
  BgraVideoRecordingSessionResult prepare_result;

  void Reset();
  bool IsConfigured() const noexcept;
};

VideoRecordingSetupResult BuildVideoRecordingPreflight(
    const VideoRecordingPreflightOptions& options,
    VideoRecordingPreflight* preflight);

VideoRecordingSetupResult BuildVideoRecordingSessionSetup(
    const VideoRecordingPreflight& preflight,
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    ID3D11DeviceContext* d3d_context,
    record::PacketStore* packet_store,
    VideoRecordingSessionSetup* setup);

const wchar_t* VideoRecordingSetupStatusName(
    VideoRecordingSetupStatus status) noexcept;

}  // namespace olouie::encode
