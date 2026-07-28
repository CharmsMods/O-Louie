#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "capture/CapturedVideoFrameSink.h"
#include "encode/VideoEncodeChain.h"
#include "performance/MultimediaThreadScheduling.h"

namespace olouie::encode {

enum class VideoEncodeThreadState {
  Idle,
  Running,
  Stopping,
  Stopped,
  Failed,
};

enum class VideoEncodeThreadCommandStatus {
  Accepted,
  AlreadyRunning,
  NotRunning,
  InvalidConfig,
  Failed,
};

struct VideoEncodeThreadOptions {
  size_t max_frames_per_batch = 4;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;

  bool IsValid() const noexcept;
};

struct VideoEncodeThreadBackend {
  std::function<bool()> is_prepared;
  std::function<capture::VideoFrameQueuePushResult(
      capture::OwnedVideoFrame)> queue_frame;
  std::function<uint32_t()> queued_frame_count;
  std::function<VideoEncodeWorkerResult(size_t)> drain_queued_frames;

  bool IsValid() const noexcept;
};

struct VideoEncodeThreadStats {
  uint64_t received_frame_count = 0;
  uint64_t accepted_frame_count = 0;
  uint64_t dropped_frame_count = 0;
  uint64_t notification_count = 0;
  uint64_t wake_count = 0;
  uint64_t drain_batch_count = 0;
  uint64_t drained_frame_count = 0;
  uint64_t last_notification_to_drain_ns = 0;
  uint64_t maximum_notification_to_drain_ns = 0;
};

struct VideoEncodeThreadSnapshot {
  VideoEncodeThreadState state = VideoEncodeThreadState::Idle;
  VideoEncodeThreadStats stats;
  VideoEncodeWorkerResult last_drain;
  VideoRecordingRuntimeFaultKind runtime_fault =
      VideoRecordingRuntimeFaultKind::None;
  performance::MultimediaThreadSchedulingSnapshot scheduling;
  std::wstring message;

  bool Failed() const noexcept;
};

struct VideoEncodeThreadCommandResult {
  VideoEncodeThreadCommandStatus status =
      VideoEncodeThreadCommandStatus::InvalidConfig;
  std::wstring message;

  bool Accepted() const noexcept;
};

class VideoEncodeThread final : public capture::ICapturedVideoFrameSink {
 public:
  VideoEncodeThread(VideoEncodeChain* chain,
                    VideoEncodeThreadOptions options);
  VideoEncodeThread(VideoEncodeThreadBackend backend,
                    VideoEncodeThreadOptions options);
  ~VideoEncodeThread();

  VideoEncodeThread(const VideoEncodeThread&) = delete;
  VideoEncodeThread& operator=(const VideoEncodeThread&) = delete;

  VideoEncodeThreadCommandResult Start();
  VideoEncodeThreadCommandResult StopAndDrain();

  capture::CapturedVideoFrameSinkResult OnCapturedVideoFrame(
      capture::OwnedVideoFrame frame) override;
  capture::ICapturedVideoFrameSink* captured_frame_sink() noexcept;

  VideoEncodeThreadSnapshot Snapshot() const;

 private:
  void Run();
  void Fail(VideoEncodeWorkerResult result, std::wstring fallback_message);

  VideoEncodeThreadBackend backend_;
  VideoEncodeThreadOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::thread thread_;
  VideoEncodeThreadState state_ = VideoEncodeThreadState::Idle;
  VideoEncodeThreadStats stats_;
  VideoEncodeWorkerResult last_drain_;
  VideoRecordingRuntimeFaultKind runtime_fault_ =
      VideoRecordingRuntimeFaultKind::None;
  performance::MultimediaThreadSchedulingSnapshot scheduling_;
  std::wstring message_;
  std::chrono::steady_clock::time_point last_notification_at_;
  std::atomic_bool accepting_{false};
  bool stop_requested_ = false;
};

const wchar_t* VideoEncodeThreadStateName(
    VideoEncodeThreadState state) noexcept;
const wchar_t* VideoEncodeThreadCommandStatusName(
    VideoEncodeThreadCommandStatus status) noexcept;

}  // namespace olouie::encode
