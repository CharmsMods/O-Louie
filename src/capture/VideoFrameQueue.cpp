#include "capture/VideoFrameQueue.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace olouie::capture {

bool OwnedVideoFrame::IsValid() const noexcept {
  return texture != nullptr && width != 0 && height != 0 &&
         timestamp_ticks >= 0;
}

bool VideoFrameQueuePushResult::Queued() const noexcept {
  return status == VideoFrameQueuePushStatus::Queued ||
         status == VideoFrameQueuePushStatus::DroppedOldestAndQueued ||
         status == VideoFrameQueuePushStatus::DroppedBacklogAndQueued;
}

bool VideoFrameQueuePushResult::Dropped() const noexcept {
  return dropped_frame_count != 0;
}

VideoFrameQueue::VideoFrameQueue(VideoFrameQueueOptions options)
    : options_(options) {}

VideoFrameQueuePushResult VideoFrameQueue::Push(OwnedVideoFrame frame) {
  if (frame.queued_at_steady_ns == 0) {
    frame.queued_at_steady_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  std::lock_guard lock(mutex_);

  if (options_.capacity == 0) {
    ++stats_.rejected_frame_count;
    return {VideoFrameQueuePushStatus::InvalidConfig};
  }
  if (!frame.IsValid()) {
    ++stats_.rejected_frame_count;
    return {VideoFrameQueuePushStatus::InvalidFrame};
  }

  latest_observed_timestamp_ticks_ =
      std::max(latest_observed_timestamp_ticks_, frame.timestamp_ticks);
  if (!frames_.empty() &&
      latest_observed_timestamp_ticks_ >= frames_.front().timestamp_ticks) {
    stats_.maximum_oldest_frame_age_ticks = std::max(
        stats_.maximum_oldest_frame_age_ticks,
        static_cast<uint64_t>(latest_observed_timestamp_ticks_ -
                              frames_.front().timestamp_ticks));
  }

  if (frames_.size() < options_.capacity) {
    frames_.push_back(std::move(frame));
    ++stats_.queued_frame_count;
    stats_.peak_depth = std::max(
        stats_.peak_depth, static_cast<uint32_t>(frames_.size()));
    return {VideoFrameQueuePushStatus::Queued};
  }

  if (options_.overflow_policy == VideoFrameOverflowPolicy::DropNewest) {
    ++stats_.dropped_newest_count;
    ++stats_.overflow_event_count;
    stats_.last_overflow_reason =
        VideoFrameQueueOverflowReason::NewestRejected;
    stats_.last_overflow_dropped_frame_count = 1;
    return {VideoFrameQueuePushStatus::DroppedNewest, 1};
  }

  if (options_.overflow_policy == VideoFrameOverflowPolicy::KeepNewest) {
    const auto dropped = static_cast<uint32_t>(frames_.size());
    frames_.clear();
    stats_.dropped_backlog_count += dropped;
    ++stats_.overflow_event_count;
    ++stats_.backlog_recovery_count;
    stats_.last_overflow_reason =
        VideoFrameQueueOverflowReason::BacklogDiscarded;
    stats_.last_overflow_dropped_frame_count = dropped;
    frames_.push_back(std::move(frame));
    ++stats_.queued_frame_count;
    return {VideoFrameQueuePushStatus::DroppedBacklogAndQueued, dropped};
  }

  frames_.pop_front();
  ++stats_.dropped_oldest_count;
  ++stats_.overflow_event_count;
  stats_.last_overflow_reason =
      VideoFrameQueueOverflowReason::OldestDiscarded;
  stats_.last_overflow_dropped_frame_count = 1;
  frames_.push_back(std::move(frame));
  ++stats_.queued_frame_count;
  return {VideoFrameQueuePushStatus::DroppedOldestAndQueued, 1};
}

bool VideoFrameQueue::TryPop(OwnedVideoFrame* frame) {
  if (frame == nullptr) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (frames_.empty()) {
    return false;
  }

  *frame = std::move(frames_.front());
  frames_.pop_front();
  ++stats_.popped_frame_count;
  return true;
}

std::vector<OwnedVideoFrame> VideoFrameQueue::Drain() {
  std::lock_guard lock(mutex_);
  std::vector<OwnedVideoFrame> drained;
  drained.reserve(frames_.size());
  while (!frames_.empty()) {
    drained.push_back(std::move(frames_.front()));
    frames_.pop_front();
    ++stats_.popped_frame_count;
  }
  return drained;
}

void VideoFrameQueue::Clear() {
  std::lock_guard lock(mutex_);
  stats_.cleared_frame_count += frames_.size();
  frames_.clear();
}

uint32_t VideoFrameQueue::Size() const {
  std::lock_guard lock(mutex_);
  return static_cast<uint32_t>(frames_.size());
}

bool VideoFrameQueue::Empty() const {
  std::lock_guard lock(mutex_);
  return frames_.empty();
}

const VideoFrameQueueOptions& VideoFrameQueue::options() const noexcept {
  return options_;
}

VideoFrameQueueStats VideoFrameQueue::stats() const {
  std::lock_guard lock(mutex_);
  auto snapshot = stats_;
  snapshot.current_depth = static_cast<uint32_t>(frames_.size());
  if (!frames_.empty()) {
    snapshot.oldest_timestamp_ticks = frames_.front().timestamp_ticks;
    snapshot.newest_timestamp_ticks = frames_.back().timestamp_ticks;
    if (latest_observed_timestamp_ticks_ >=
        snapshot.oldest_timestamp_ticks) {
      snapshot.current_oldest_frame_age_ticks =
          static_cast<uint64_t>(latest_observed_timestamp_ticks_ -
                                snapshot.oldest_timestamp_ticks);
    }
  }
  return snapshot;
}

const wchar_t* VideoFrameQueuePushStatusName(
    VideoFrameQueuePushStatus status) noexcept {
  switch (status) {
    case VideoFrameQueuePushStatus::Queued:
      return L"queued";
    case VideoFrameQueuePushStatus::DroppedNewest:
      return L"dropped newest";
    case VideoFrameQueuePushStatus::DroppedOldestAndQueued:
      return L"dropped oldest and queued";
    case VideoFrameQueuePushStatus::DroppedBacklogAndQueued:
      return L"dropped backlog and queued newest";
    case VideoFrameQueuePushStatus::InvalidFrame:
      return L"invalid frame";
    case VideoFrameQueuePushStatus::InvalidConfig:
      return L"invalid config";
  }

  return L"unknown";
}

const wchar_t* VideoFrameQueueOverflowReasonName(
    VideoFrameQueueOverflowReason reason) noexcept {
  switch (reason) {
    case VideoFrameQueueOverflowReason::None:
      return L"none";
    case VideoFrameQueueOverflowReason::NewestRejected:
      return L"newest rejected";
    case VideoFrameQueueOverflowReason::OldestDiscarded:
      return L"oldest discarded";
    case VideoFrameQueueOverflowReason::BacklogDiscarded:
      return L"stale backlog discarded";
  }
  return L"unknown";
}

}  // namespace olouie::capture
