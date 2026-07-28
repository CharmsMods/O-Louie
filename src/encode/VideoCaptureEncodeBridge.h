#pragma once

#include <cstdint>
#include <string>

#include "capture/CapturedVideoFrameSink.h"
#include "encode/VideoEncodeChain.h"

namespace olouie::encode {

struct VideoCaptureEncodeBridgeOptions {
  int64_t timestamp_frequency = 0;
  bool start_timebase_on_first_frame = false;
};

struct VideoCaptureEncodeBridgeStats {
  uint64_t received_frame_count = 0;
  uint64_t queued_frame_count = 0;
  uint64_t dropped_frame_count = 0;
  uint64_t invalid_frame_count = 0;
  uint64_t invalid_config_count = 0;
  uint64_t sink_error_count = 0;
  uint64_t timebase_start_count = 0;
};

class VideoCaptureEncodeBridge final
    : public capture::ICapturedVideoFrameSink {
 public:
  explicit VideoCaptureEncodeBridge(
      VideoEncodeChain* chain,
      VideoCaptureEncodeBridgeOptions options = {});

  VideoCaptureEncodeBridge(const VideoCaptureEncodeBridge&) = delete;
  VideoCaptureEncodeBridge& operator=(const VideoCaptureEncodeBridge&) =
      delete;

  bool IsConfigured() const noexcept;

  capture::ICapturedVideoFrameSink* captured_frame_sink() noexcept;

  capture::CapturedVideoFrameSinkResult OnCapturedVideoFrame(
      capture::OwnedVideoFrame frame) override;

  VideoEncodeWorkerResult DrainQueuedFrames();
  VideoEncodeWorkerResult DrainQueuedFrames(size_t max_frames);
  VideoEncodeWorkerResult DrainAllQueuedFrames();

  const VideoCaptureEncodeBridgeOptions& options() const noexcept;
  const VideoCaptureEncodeBridgeStats& stats() const noexcept;
  const capture::VideoFrameQueuePushResult& last_queue_result()
      const noexcept;
  int64_t first_timestamp_ticks() const noexcept;

 private:
  capture::CapturedVideoFrameSinkResult EnsureFrameTimebase(
      int64_t timestamp_ticks);
  capture::CapturedVideoFrameSinkResult Result(
      capture::CapturedVideoFrameSinkStatus status,
      std::wstring message,
      uint32_t dropped_frame_count = 0) const;

  VideoEncodeChain* chain_ = nullptr;
  VideoCaptureEncodeBridgeOptions options_;
  VideoCaptureEncodeBridgeStats stats_;
  capture::VideoFrameQueuePushResult last_queue_result_;
  bool timebase_started_ = false;
  int64_t first_timestamp_ticks_ = 0;
};

}  // namespace olouie::encode
