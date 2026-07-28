#pragma once

#include <windows.h>

#include <d3d11.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "capture/WgcMonitorCapture.h"
#include "encode/VideoCaptureEncodeBridge.h"
#include "encode/VideoEncodeChain.h"

namespace olouie::encode {

using WgcVideoFrameCopyRunner = capture::WgcFrameCopySmokeResult (*)(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames,
    capture::ICapturedVideoFrameSink* sink);

struct VideoLiveCaptureEncodeOptions {
  std::chrono::milliseconds duration{0};
  std::chrono::milliseconds drain_interval{10};
  uint32_t max_copied_frames = 0;
  size_t max_frames_per_drain_tick = 4;
  int64_t timestamp_frequency = 10000000;
  bool start_timebase_on_first_frame = true;
  int64_t timebase_origin_ticks = 0;
  bool use_explicit_timebase_origin = false;
};

enum class VideoLiveCaptureEncodeStatus {
  Success,
  InvalidConfig,
  CaptureFailed,
  DrainFailed,
};

struct VideoLiveCaptureEncodeResult {
  bool bridge_configured = false;
  uint32_t drain_tick_count = 0;
  uint64_t drained_frame_count = 0;
  capture::WgcFrameCopySmokeResult capture;
  VideoEncodeWorkerResult last_tick_drain;
  VideoEncodeWorkerResult final_drain;
  VideoCaptureEncodeBridgeStats bridge_stats;
  capture::VideoFrameQueueStats queue_stats;
  BgraVideoRecordingSessionStats session_stats;
  int64_t first_timestamp_ticks = 0;
};

struct VideoLiveCaptureEncodeRunResult {
  VideoLiveCaptureEncodeStatus status =
      VideoLiveCaptureEncodeStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

VideoLiveCaptureEncodeRunResult RunWgcVideoLiveCaptureEncode(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const VideoLiveCaptureEncodeOptions& options,
    VideoEncodeChain* chain,
    VideoLiveCaptureEncodeResult* result);

VideoLiveCaptureEncodeRunResult RunWgcVideoLiveCaptureEncode(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const VideoLiveCaptureEncodeOptions& options,
    VideoEncodeChain* chain,
    WgcVideoFrameCopyRunner capture_runner,
    VideoLiveCaptureEncodeResult* result);

const wchar_t* VideoLiveCaptureEncodeStatusName(
    VideoLiveCaptureEncodeStatus status) noexcept;

}  // namespace olouie::encode
