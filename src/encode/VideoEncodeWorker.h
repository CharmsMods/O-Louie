#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "capture/VideoFrameQueue.h"
#include "encode/VideoRecordingSession.h"
#include "record/Timebase.h"

namespace olouie::encode {

enum class VideoEncodeWorkerStatus {
  Success,
  InvalidArgument,
  FrameTimingError,
  FrameFormatMismatch,
  SessionError,
};

struct VideoEncodeWorkerOptions {
  record::Timebase timebase;
  int64_t fallback_frame_duration_ns = 0;
};

struct VideoEncodeWorkerResult {
  VideoEncodeWorkerStatus status = VideoEncodeWorkerStatus::InvalidArgument;
  std::wstring message;
  size_t popped_frame_count = 0;
  size_t processed_frame_count = 0;
  size_t failed_frame_count = 0;
  uint32_t remaining_frame_count = 0;
  uint64_t dropped_frame_count = 0;
  uint64_t dropped_frame_count_delta = 0;
  BgraVideoRecordingSessionResult first_failure;

  bool Succeeded() const noexcept;
};

struct VideoEncodeWorkerStats {
  uint64_t popped_frame_count = 0;
  uint64_t processed_frame_count = 0;
  uint64_t failed_frame_count = 0;
  uint64_t timing_error_count = 0;
  uint64_t format_mismatch_count = 0;
  uint64_t session_error_count = 0;
  uint64_t queue_wait_sample_count = 0;
  uint64_t last_queue_wait_ns = 0;
  uint64_t maximum_queue_wait_ns = 0;
  uint64_t total_queue_wait_ns = 0;
};

class VideoEncodeWorker final {
 public:
  VideoEncodeWorker(capture::VideoFrameQueue* queue,
                    BgraVideoRecordingSession* session,
                    VideoEncodeWorkerOptions options);

  VideoEncodeWorkerResult DrainQueuedFrames(size_t max_frames);
  VideoEncodeWorkerResult DrainAllQueuedFrames();
  void SetOptions(VideoEncodeWorkerOptions options);

  const VideoEncodeWorkerOptions& options() const noexcept;
  const VideoEncodeWorkerStats& stats() const noexcept;

 private:
  capture::VideoFrameQueue* queue_ = nullptr;
  BgraVideoRecordingSession* session_ = nullptr;
  VideoEncodeWorkerOptions options_;
  VideoEncodeWorkerStats stats_;
};

const wchar_t* VideoEncodeWorkerStatusName(
    VideoEncodeWorkerStatus status) noexcept;

}  // namespace olouie::encode
