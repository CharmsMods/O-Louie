#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "capture/BgraTexturePool.h"

namespace olouie::capture {

struct OwnedVideoFrame {
  BgraTexturePoolLease texture_pool_lease;
  winrt::com_ptr<ID3D11Texture2D> texture;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t timestamp_ticks = 0;
  uint64_t queued_at_steady_ns = 0;

  bool IsValid() const noexcept;
};

enum class VideoFrameOverflowPolicy {
  DropNewest,
  DropOldest,
  KeepNewest,
};

enum class VideoFrameQueuePushStatus {
  Queued,
  DroppedNewest,
  DroppedOldestAndQueued,
  DroppedBacklogAndQueued,
  InvalidFrame,
  InvalidConfig,
};

enum class VideoFrameQueueOverflowReason {
  None,
  NewestRejected,
  OldestDiscarded,
  BacklogDiscarded,
};

struct VideoFrameQueueOptions {
  uint32_t capacity = 0;
  VideoFrameOverflowPolicy overflow_policy =
      VideoFrameOverflowPolicy::DropNewest;
};

struct VideoFrameQueueStats {
  uint64_t queued_frame_count = 0;
  uint64_t popped_frame_count = 0;
  uint64_t dropped_newest_count = 0;
  uint64_t dropped_oldest_count = 0;
  uint64_t dropped_backlog_count = 0;
  uint64_t overflow_event_count = 0;
  uint64_t backlog_recovery_count = 0;
  uint64_t rejected_frame_count = 0;
  uint64_t cleared_frame_count = 0;
  uint32_t current_depth = 0;
  uint32_t peak_depth = 0;
  int64_t oldest_timestamp_ticks = 0;
  int64_t newest_timestamp_ticks = 0;
  uint64_t current_oldest_frame_age_ticks = 0;
  uint64_t maximum_oldest_frame_age_ticks = 0;
  uint32_t last_overflow_dropped_frame_count = 0;
  VideoFrameQueueOverflowReason last_overflow_reason =
      VideoFrameQueueOverflowReason::None;
};

struct VideoFrameQueuePushResult {
  VideoFrameQueuePushStatus status =
      VideoFrameQueuePushStatus::InvalidConfig;
  uint32_t dropped_frame_count = 0;

  bool Queued() const noexcept;
  bool Dropped() const noexcept;
};

class VideoFrameQueue final {
 public:
  explicit VideoFrameQueue(VideoFrameQueueOptions options);

  VideoFrameQueue(const VideoFrameQueue&) = delete;
  VideoFrameQueue& operator=(const VideoFrameQueue&) = delete;

  VideoFrameQueuePushResult Push(OwnedVideoFrame frame);
  bool TryPop(OwnedVideoFrame* frame);
  std::vector<OwnedVideoFrame> Drain();
  void Clear();

  uint32_t Size() const;
  bool Empty() const;
  const VideoFrameQueueOptions& options() const noexcept;
  VideoFrameQueueStats stats() const;

 private:
  VideoFrameQueueOptions options_;
  mutable std::mutex mutex_;
  std::deque<OwnedVideoFrame> frames_;
  VideoFrameQueueStats stats_;
  int64_t latest_observed_timestamp_ticks_ = 0;
};

const wchar_t* VideoFrameQueuePushStatusName(
    VideoFrameQueuePushStatus status) noexcept;
const wchar_t* VideoFrameQueueOverflowReasonName(
    VideoFrameQueueOverflowReason reason) noexcept;

}  // namespace olouie::capture
