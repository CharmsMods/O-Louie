#pragma once

#include <cstddef>
#include <mutex>

#include "capture/VideoFrameQueue.h"
#include "encode/VideoEncodeWorker.h"
#include "encode/VideoRecordingSession.h"

namespace olouie::encode {

struct VideoEncodeChainConfig {
  capture::VideoFrameQueueOptions queue_options;
  BgraVideoRecordingSessionOptions session_options;
  VideoEncodeWorkerOptions worker_options;
  size_t drain_frame_budget = 0;

  bool IsValid() const noexcept;
};

struct VideoEncodeChainRuntimeSnapshot {
  capture::VideoFrameQueueStats queue;
  VideoEncodeWorkerStats worker;
  BgraVideoRecordingSessionStats session;
  H264PacketStoreConfig h264;
  graphics::GpuBgraToNv12Plan conversion_plan;
  graphics::GpuBgraToNv12ConverterStats converter;
  MfHardwareH264EncoderSessionInfo encoder;
};

class VideoEncodeChain final {
 public:
  explicit VideoEncodeChain(VideoEncodeChainConfig config);

  VideoEncodeChain(const VideoEncodeChain&) = delete;
  VideoEncodeChain& operator=(const VideoEncodeChain&) = delete;

  BgraVideoRecordingSessionResult Prepare(
      MfHardwareH264EncoderSession* encoder_session,
      ID3D11Device* d3d_device,
      ID3D11DeviceContext* d3d_context,
      record::PacketStore* packet_store);
  capture::VideoFrameQueuePushResult QueueFrame(
      capture::OwnedVideoFrame frame);
  VideoEncodeWorkerResult DrainQueuedFrames();
  VideoEncodeWorkerResult DrainQueuedFrames(size_t max_frames);
  VideoEncodeWorkerResult DrainAllQueuedFrames();
  BgraVideoRecordingSessionResult FinalizeEncoder();
  void SetWorkerOptions(VideoEncodeWorkerOptions options);
  void ClearQueuedFrames();

  bool IsConfigured() const noexcept;
  bool IsPrepared() const noexcept;
  uint32_t queued_frame_count() const;
  bool queue_empty() const;

  const VideoEncodeChainConfig& config() const noexcept;
  const BgraVideoRecordingSessionResult& prepare_result() const noexcept;
  capture::VideoFrameQueueStats queue_stats() const;
  const VideoEncodeWorkerStats& worker_stats() const noexcept;
  const BgraVideoRecordingSessionStats& session_stats() const noexcept;
  const H264PacketStoreConfig& session_config() const noexcept;
  const graphics::GpuBgraToNv12Plan& conversion_plan() const noexcept;
  const graphics::GpuBgraToNv12ConverterStats& converter_stats()
      const noexcept;
  VideoEncodeChainRuntimeSnapshot SnapshotRuntime() const;

 private:
  void RefreshRuntimeSnapshotLocked();

  VideoEncodeChainConfig config_;
  capture::VideoFrameQueue queue_;
  BgraVideoRecordingSession session_;
  VideoEncodeWorker worker_;
  BgraVideoRecordingSessionResult prepare_result_;
  mutable std::mutex processing_mutex_;
  mutable std::mutex runtime_snapshot_mutex_;
  VideoEncodeChainRuntimeSnapshot runtime_snapshot_;
};

}  // namespace olouie::encode
