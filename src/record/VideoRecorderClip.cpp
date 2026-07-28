#include "record/VideoRecorderClip.h"

#include <limits>
#include <utility>

namespace olouie::record {

bool VideoRecorderClipCommandResult::Accepted() const noexcept {
  return status == VideoRecorderClipCommandStatus::Accepted;
}

VideoRecorderClipCommandQueue::VideoRecorderClipCommandQueue(size_t capacity)
    : capacity_(capacity) {}

VideoRecorderClipCommandResult VideoRecorderClipCommandQueue::Enqueue(
    std::chrono::milliseconds duration) {
  if (duration <= std::chrono::milliseconds(0)) {
    return {VideoRecorderClipCommandStatus::InvalidDuration, 0,
            L"Clip duration must be greater than zero."};
  }

  VideoRecorderClipRequest request;
  request.kind = VideoRecorderExportKind::Clip;
  request.duration = duration;
  return EnqueueRequest(std::move(request));
}

VideoRecorderClipCommandResult
VideoRecorderClipCommandQueue::EnqueueBookmark(
    std::chrono::milliseconds pre_roll,
    std::chrono::milliseconds post_roll) {
  if (pre_roll < std::chrono::milliseconds(0) ||
      post_roll < std::chrono::milliseconds(0) ||
      (pre_roll == std::chrono::milliseconds(0) &&
       post_roll == std::chrono::milliseconds(0)) ||
      pre_roll.count() >
          std::numeric_limits<int64_t>::max() - post_roll.count()) {
    return {VideoRecorderClipCommandStatus::InvalidDuration, 0,
            L"Bookmark export durations are invalid."};
  }

  VideoRecorderClipRequest request;
  request.kind = VideoRecorderExportKind::Bookmark;
  request.duration = pre_roll + post_roll;
  request.bookmark_pre_roll = pre_roll;
  request.bookmark_post_roll = post_roll;
  return EnqueueRequest(std::move(request));
}

VideoRecorderClipCommandResult
VideoRecorderClipCommandQueue::EnqueueRequest(
    VideoRecorderClipRequest request) {
  if (request.duration <= std::chrono::milliseconds(0)) {
    return {VideoRecorderClipCommandStatus::InvalidDuration, 0,
            L"Export duration must be greater than zero."};
  }

  std::lock_guard lock(mutex_);
  if (shutting_down_) {
    return {VideoRecorderClipCommandStatus::ShuttingDown, 0,
            L"Clip command queue is shutting down."};
  }
  if (capacity_ == 0 || pending_.size() >= capacity_) {
    return {VideoRecorderClipCommandStatus::QueueFull, 0,
            L"Too many clip requests are already queued."};
  }

  const uint64_t request_id = next_request_id_++;
  request.request_id = request_id;
  pending_.push_back(std::move(request));
  return {VideoRecorderClipCommandStatus::Accepted, request_id, L""};
}

bool VideoRecorderClipCommandQueue::TryPop(
    VideoRecorderClipRequest* request) {
  if (request == nullptr) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (pending_.empty()) {
    return false;
  }
  *request = pending_.front();
  pending_.pop_front();
  return true;
}

void VideoRecorderClipCommandQueue::Shutdown() {
  std::lock_guard lock(mutex_);
  shutting_down_ = true;
  pending_.clear();
}

size_t VideoRecorderClipCommandQueue::pending_count() const {
  std::lock_guard lock(mutex_);
  return pending_.size();
}

size_t VideoRecorderClipCommandQueue::capacity() const noexcept {
  return capacity_;
}

const wchar_t* VideoRecorderClipStateName(
    VideoRecorderClipState state) noexcept {
  switch (state) {
    case VideoRecorderClipState::None:
      return L"none";
    case VideoRecorderClipState::Queued:
      return L"queued";
    case VideoRecorderClipState::Saved:
      return L"saved";
    case VideoRecorderClipState::Failed:
      return L"failed";
  }
  return L"unknown";
}

const wchar_t* VideoRecorderExportKindName(
    VideoRecorderExportKind kind) noexcept {
  switch (kind) {
    case VideoRecorderExportKind::Clip:
      return L"clip";
    case VideoRecorderExportKind::Bookmark:
      return L"bookmark";
  }
  return L"unknown";
}

const wchar_t* VideoRecorderClipCommandStatusName(
    VideoRecorderClipCommandStatus status) noexcept {
  switch (status) {
    case VideoRecorderClipCommandStatus::Accepted:
      return L"accepted";
    case VideoRecorderClipCommandStatus::InvalidDuration:
      return L"invalid duration";
    case VideoRecorderClipCommandStatus::QueueFull:
      return L"queue full";
    case VideoRecorderClipCommandStatus::ShuttingDown:
      return L"shutting down";
  }
  return L"unknown";
}

}  // namespace olouie::record
