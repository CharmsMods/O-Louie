#include "encode/VideoEncodeWorker.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

namespace olouie::encode {
namespace {

uint64_t DroppedFrameCount(const capture::VideoFrameQueueStats& stats) {
  return stats.dropped_newest_count + stats.dropped_oldest_count +
         stats.dropped_backlog_count;
}

VideoEncodeWorkerResult Result(
    VideoEncodeWorkerStatus status,
    std::wstring message,
    size_t popped_frame_count,
    size_t processed_frame_count,
    size_t failed_frame_count,
    const capture::VideoFrameQueueStats* before_stats,
    const capture::VideoFrameQueue* queue) {
  VideoEncodeWorkerResult result;
  result.status = status;
  result.message = std::move(message);
  result.popped_frame_count = popped_frame_count;
  result.processed_frame_count = processed_frame_count;
  result.failed_frame_count = failed_frame_count;

  if (queue != nullptr) {
    const auto after_stats = queue->stats();
    const auto after_dropped = DroppedFrameCount(after_stats);
    result.remaining_frame_count = queue->Size();
    result.dropped_frame_count = after_dropped;
    if (before_stats != nullptr) {
      const auto before_dropped = DroppedFrameCount(*before_stats);
      if (after_dropped >= before_dropped) {
        result.dropped_frame_count_delta = after_dropped - before_dropped;
      }
    }
  }

  return result;
}

}  // namespace

bool VideoEncodeWorkerResult::Succeeded() const noexcept {
  return status == VideoEncodeWorkerStatus::Success;
}

VideoEncodeWorker::VideoEncodeWorker(capture::VideoFrameQueue* queue,
                                     BgraVideoRecordingSession* session,
                                     VideoEncodeWorkerOptions options)
    : queue_(queue), session_(session), options_(std::move(options)) {}

VideoEncodeWorkerResult VideoEncodeWorker::DrainQueuedFrames(
    size_t max_frames) {
  capture::VideoFrameQueueStats before_stats{};
  const capture::VideoFrameQueueStats* before_stats_ptr = nullptr;
  if (queue_ != nullptr) {
    before_stats = queue_->stats();
    before_stats_ptr = &before_stats;
  }

  if (queue_ == nullptr || session_ == nullptr) {
    return Result(VideoEncodeWorkerStatus::InvalidArgument,
                  L"Video encode worker needs a frame queue and BGRA video "
                  L"recording session.",
                  0, 0, 0, before_stats_ptr, queue_);
  }

  if (!options_.timebase.IsValid() ||
      options_.fallback_frame_duration_ns <= 0) {
    return Result(VideoEncodeWorkerStatus::InvalidArgument,
                  L"Video encode worker needs a valid timebase and positive "
                  L"fallback frame duration.",
                  0, 0, 0, before_stats_ptr, queue_);
  }

  const auto& session_options = session_->options();
  if (session_options.source_width == 0 ||
      session_options.source_height == 0) {
    return Result(VideoEncodeWorkerStatus::InvalidArgument,
                  L"Video encode worker needs a BGRA video session with "
                  L"source dimensions.",
                  0, 0, 0, before_stats_ptr, queue_);
  }

  size_t popped_frames = 0;
  size_t processed_frames = 0;

  while (popped_frames < max_frames) {
    capture::OwnedVideoFrame frame;
    if (!queue_->TryPop(&frame)) {
      break;
    }

    ++popped_frames;
    ++stats_.popped_frame_count;
    if (frame.queued_at_steady_ns != 0) {
      const auto now_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      const auto queue_wait_ns =
          now_ns >= frame.queued_at_steady_ns
              ? now_ns - frame.queued_at_steady_ns
              : 0;
      ++stats_.queue_wait_sample_count;
      stats_.last_queue_wait_ns = queue_wait_ns;
      stats_.maximum_queue_wait_ns =
          std::max(stats_.maximum_queue_wait_ns, queue_wait_ns);
      stats_.total_queue_wait_ns += queue_wait_ns;
    }

    if (frame.width != session_options.source_width ||
        frame.height != session_options.source_height) {
      ++stats_.failed_frame_count;
      ++stats_.format_mismatch_count;
      return Result(VideoEncodeWorkerStatus::FrameFormatMismatch,
                    L"Queued BGRA frame dimensions do not match the video "
                    L"recording session source dimensions.",
                    popped_frames, processed_frames, 1, before_stats_ptr,
                    queue_);
    }

    const int64_t pts_ns = options_.timebase.QpcToNs(frame.timestamp_ticks);
    if (pts_ns < 0) {
      ++stats_.failed_frame_count;
      ++stats_.timing_error_count;
      return Result(VideoEncodeWorkerStatus::FrameTimingError,
                    L"Queued BGRA frame timestamp is before the video "
                    L"session timebase start.",
                    popped_frames, processed_frames, 1, before_stats_ptr,
                    queue_);
    }

    auto submit = session_->SubmitBgraFrame(
        frame.texture.get(), pts_ns, options_.fallback_frame_duration_ns);
    if (!submit.Succeeded()) {
      ++stats_.failed_frame_count;
      ++stats_.session_error_count;

      auto result =
          Result(VideoEncodeWorkerStatus::SessionError, submit.message,
                 popped_frames, processed_frames, 1, before_stats_ptr, queue_);
      result.first_failure = std::move(submit);
      return result;
    }

    ++processed_frames;
    ++stats_.processed_frame_count;
  }

  return Result(VideoEncodeWorkerStatus::Success, L"", popped_frames,
                processed_frames, 0, before_stats_ptr, queue_);
}

VideoEncodeWorkerResult VideoEncodeWorker::DrainAllQueuedFrames() {
  return DrainQueuedFrames(std::numeric_limits<size_t>::max());
}

void VideoEncodeWorker::SetOptions(VideoEncodeWorkerOptions options) {
  options_ = std::move(options);
}

const VideoEncodeWorkerOptions& VideoEncodeWorker::options() const noexcept {
  return options_;
}

const VideoEncodeWorkerStats& VideoEncodeWorker::stats() const noexcept {
  return stats_;
}

const wchar_t* VideoEncodeWorkerStatusName(
    VideoEncodeWorkerStatus status) noexcept {
  switch (status) {
    case VideoEncodeWorkerStatus::Success:
      return L"success";
    case VideoEncodeWorkerStatus::InvalidArgument:
      return L"invalid argument";
    case VideoEncodeWorkerStatus::FrameTimingError:
      return L"frame timing error";
    case VideoEncodeWorkerStatus::FrameFormatMismatch:
      return L"frame format mismatch";
    case VideoEncodeWorkerStatus::SessionError:
      return L"session error";
  }

  return L"unknown";
}

}  // namespace olouie::encode
