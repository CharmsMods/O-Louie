#include "encode/VideoEncodeThread.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace olouie::encode {
namespace {

VideoEncodeThreadCommandResult CommandResult(
    VideoEncodeThreadCommandStatus status,
    std::wstring message = {}) {
  return {status, std::move(message)};
}

capture::CapturedVideoFrameSinkResult SinkResult(
    capture::CapturedVideoFrameSinkStatus status,
    std::wstring message = {},
    uint32_t dropped_frame_count = 0) {
  capture::CapturedVideoFrameSinkResult result;
  result.status = status;
  result.message = std::move(message);
  result.dropped_frame_count = dropped_frame_count;
  return result;
}

capture::CapturedVideoFrameSinkStatus SinkStatusFromQueue(
    capture::VideoFrameQueuePushStatus status) noexcept {
  switch (status) {
    case capture::VideoFrameQueuePushStatus::Queued:
      return capture::CapturedVideoFrameSinkStatus::Success;
    case capture::VideoFrameQueuePushStatus::DroppedNewest:
      return capture::CapturedVideoFrameSinkStatus::Dropped;
    case capture::VideoFrameQueuePushStatus::DroppedOldestAndQueued:
    case capture::VideoFrameQueuePushStatus::DroppedBacklogAndQueued:
      return capture::CapturedVideoFrameSinkStatus::DroppedAndQueued;
    case capture::VideoFrameQueuePushStatus::InvalidFrame:
      return capture::CapturedVideoFrameSinkStatus::InvalidFrame;
    case capture::VideoFrameQueuePushStatus::InvalidConfig:
      return capture::CapturedVideoFrameSinkStatus::InvalidConfig;
  }
  return capture::CapturedVideoFrameSinkStatus::SinkError;
}

uint64_t ElapsedNanoseconds(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) noexcept {
  if (start == std::chrono::steady_clock::time_point{} || end < start) {
    return 0;
  }
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

}  // namespace

bool VideoEncodeThreadOptions::IsValid() const noexcept {
  return max_frames_per_batch != 0 &&
         (performance_mode ==
              performance::CapturePerformanceMode::Balanced ||
          performance_mode ==
              performance::CapturePerformanceMode::CaptureFirst);
}

bool VideoEncodeThreadBackend::IsValid() const noexcept {
  return is_prepared && queue_frame && queued_frame_count &&
         drain_queued_frames;
}

bool VideoEncodeThreadSnapshot::Failed() const noexcept {
  return state == VideoEncodeThreadState::Failed;
}

bool VideoEncodeThreadCommandResult::Accepted() const noexcept {
  return status == VideoEncodeThreadCommandStatus::Accepted;
}

VideoEncodeThread::VideoEncodeThread(VideoEncodeChain* chain,
                                     VideoEncodeThreadOptions options)
    : VideoEncodeThread(
          VideoEncodeThreadBackend{
              [chain] { return chain != nullptr && chain->IsPrepared(); },
              [chain](capture::OwnedVideoFrame frame) {
                return chain == nullptr
                           ? capture::VideoFrameQueuePushResult{
                                 capture::VideoFrameQueuePushStatus::InvalidConfig}
                           : chain->QueueFrame(std::move(frame));
              },
              [chain] {
                return chain == nullptr ? 0u : chain->queued_frame_count();
              },
              [chain](size_t max_frames) {
                if (chain != nullptr) {
                  return chain->DrainQueuedFrames(max_frames);
                }
                VideoEncodeWorkerResult result;
                result.status = VideoEncodeWorkerStatus::InvalidArgument;
                result.message = L"Video encode chain is unavailable.";
                return result;
              }},
          options) {}

VideoEncodeThread::VideoEncodeThread(VideoEncodeThreadBackend backend,
                                     VideoEncodeThreadOptions options)
    : backend_(std::move(backend)), options_(options) {}

VideoEncodeThread::~VideoEncodeThread() {
  (void)StopAndDrain();
}

VideoEncodeThreadCommandResult VideoEncodeThread::Start() {
  std::lock_guard lock(mutex_);
  if (state_ == VideoEncodeThreadState::Running ||
      state_ == VideoEncodeThreadState::Stopping) {
    return CommandResult(VideoEncodeThreadCommandStatus::AlreadyRunning,
                         L"Video encode thread is already running.");
  }
  if (!backend_.IsValid() || !options_.IsValid() ||
      !backend_.is_prepared() ||
      thread_.joinable()) {
    return CommandResult(VideoEncodeThreadCommandStatus::InvalidConfig,
                         L"Video encode thread needs a prepared chain and "
                         L"valid batch size.");
  }

  stop_requested_ = false;
  stats_ = {};
  last_drain_ = {};
  runtime_fault_ = VideoRecordingRuntimeFaultKind::None;
  scheduling_ = {};
  message_.clear();
  last_notification_at_ = {};
  state_ = VideoEncodeThreadState::Running;
  accepting_.store(true);
  try {
    thread_ = std::thread(&VideoEncodeThread::Run, this);
  } catch (const std::exception&) {
    accepting_.store(false);
    state_ = VideoEncodeThreadState::Failed;
    message_ = L"Could not create the video encode thread.";
    return CommandResult(VideoEncodeThreadCommandStatus::Failed, message_);
  }
  return CommandResult(VideoEncodeThreadCommandStatus::Accepted);
}

VideoEncodeThreadCommandResult VideoEncodeThread::StopAndDrain() {
  {
    std::lock_guard lock(mutex_);
    accepting_.store(false);
    if (state_ == VideoEncodeThreadState::Idle ||
        (state_ == VideoEncodeThreadState::Stopped && !thread_.joinable())) {
      return CommandResult(VideoEncodeThreadCommandStatus::NotRunning);
    }
    stop_requested_ = true;
    if (state_ == VideoEncodeThreadState::Running) {
      state_ = VideoEncodeThreadState::Stopping;
    }
  }
  ready_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }

  std::lock_guard lock(mutex_);
  if (state_ == VideoEncodeThreadState::Failed) {
    return CommandResult(VideoEncodeThreadCommandStatus::Failed, message_);
  }
  state_ = VideoEncodeThreadState::Stopped;
  return CommandResult(VideoEncodeThreadCommandStatus::Accepted);
}

capture::CapturedVideoFrameSinkResult
VideoEncodeThread::OnCapturedVideoFrame(capture::OwnedVideoFrame frame) {
  if (!frame.IsValid()) {
    return SinkResult(capture::CapturedVideoFrameSinkStatus::InvalidFrame,
                      L"Captured video frame is invalid.");
  }
  capture::VideoFrameQueuePushResult queued;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_.load() || !backend_.IsValid()) {
      return SinkResult(capture::CapturedVideoFrameSinkStatus::SinkError,
                        L"Video encode thread is not accepting frames.");
    }
    ++stats_.received_frame_count;
    queued = backend_.queue_frame(std::move(frame));
  }
  const auto sink_status = SinkStatusFromQueue(queued.status);
  if (sink_status == capture::CapturedVideoFrameSinkStatus::Success ||
      sink_status == capture::CapturedVideoFrameSinkStatus::DroppedAndQueued) {
    {
      std::lock_guard lock(mutex_);
      ++stats_.accepted_frame_count;
      stats_.dropped_frame_count += queued.dropped_frame_count;
      ++stats_.notification_count;
      last_notification_at_ = std::chrono::steady_clock::now();
    }
    ready_.notify_one();
  } else if (sink_status == capture::CapturedVideoFrameSinkStatus::Dropped) {
    std::lock_guard lock(mutex_);
    stats_.dropped_frame_count +=
        queued.dropped_frame_count == 0 ? 1 : queued.dropped_frame_count;
  }

  std::wstring message;
  if (sink_status == capture::CapturedVideoFrameSinkStatus::InvalidFrame) {
    message = L"Video encode queue rejected an invalid frame.";
  } else if (sink_status ==
             capture::CapturedVideoFrameSinkStatus::InvalidConfig) {
    message = L"Video encode queue is not configured.";
  } else if (sink_status == capture::CapturedVideoFrameSinkStatus::SinkError) {
    message = L"Video encode queue returned an unexpected status.";
  }
  return SinkResult(sink_status, std::move(message),
                    queued.dropped_frame_count);
}

capture::ICapturedVideoFrameSink*
VideoEncodeThread::captured_frame_sink() noexcept {
  return this;
}

VideoEncodeThreadSnapshot VideoEncodeThread::Snapshot() const {
  std::lock_guard lock(mutex_);
  VideoEncodeThreadSnapshot snapshot;
  snapshot.state = state_;
  snapshot.stats = stats_;
  snapshot.last_drain = last_drain_;
  snapshot.runtime_fault = runtime_fault_;
  snapshot.scheduling = scheduling_;
  snapshot.message = message_;
  return snapshot;
}

void VideoEncodeThread::Run() {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_initialized = SUCCEEDED(com_result);
  if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
    VideoEncodeWorkerResult failure;
    failure.status = VideoEncodeWorkerStatus::InvalidArgument;
    failure.message = L"Video encode thread COM initialization failed.";
    Fail(std::move(failure), L"Video encode thread could not initialize COM.");
    return;
  }

  performance::MultimediaThreadRegistration scheduling;
  const auto scheduling_snapshot = scheduling.Register(
      performance::BuildMultimediaThreadSchedulingPlan(
          options_.performance_mode,
          performance::MultimediaThreadWorkload::VideoEncode));
  {
    std::lock_guard lock(mutex_);
    scheduling_ = scheduling_snapshot;
  }

  for (;;) {
    std::chrono::steady_clock::time_point notification_at;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] {
        return stop_requested_ ||
               (backend_.IsValid() && backend_.queued_frame_count() != 0);
      });
      if (stop_requested_ &&
          (!backend_.IsValid() || backend_.queued_frame_count() == 0)) {
        break;
      }
      notification_at = last_notification_at_;
      ++stats_.wake_count;
    }

    const auto drain_started = std::chrono::steady_clock::now();
    auto drained =
        backend_.drain_queued_frames(options_.max_frames_per_batch);
    const auto notification_latency =
        ElapsedNanoseconds(notification_at, drain_started);
    {
      std::lock_guard lock(mutex_);
      last_drain_ = drained;
      ++stats_.drain_batch_count;
      stats_.drained_frame_count += drained.processed_frame_count;
      stats_.last_notification_to_drain_ns = notification_latency;
      stats_.maximum_notification_to_drain_ns = std::max(
          stats_.maximum_notification_to_drain_ns, notification_latency);
    }
    if (!drained.Succeeded()) {
      Fail(std::move(drained), L"Video encode worker failed.");
      break;
    }
  }

  if (com_initialized) {
    CoUninitialize();
  }
  std::lock_guard lock(mutex_);
  if (state_ != VideoEncodeThreadState::Failed) {
    state_ = VideoEncodeThreadState::Stopped;
  }
}

void VideoEncodeThread::Fail(VideoEncodeWorkerResult result,
                             std::wstring fallback_message) {
  accepting_.store(false);
  std::lock_guard lock(mutex_);
  runtime_fault_ = result.first_failure.RuntimeFaultKind();
  message_ = result.message.empty() ? std::move(fallback_message)
                                    : result.message;
  last_drain_ = std::move(result);
  state_ = VideoEncodeThreadState::Failed;
  stop_requested_ = true;
}

const wchar_t* VideoEncodeThreadStateName(
    VideoEncodeThreadState state) noexcept {
  switch (state) {
    case VideoEncodeThreadState::Idle:
      return L"idle";
    case VideoEncodeThreadState::Running:
      return L"running";
    case VideoEncodeThreadState::Stopping:
      return L"stopping";
    case VideoEncodeThreadState::Stopped:
      return L"stopped";
    case VideoEncodeThreadState::Failed:
      return L"failed";
  }
  return L"unknown";
}

const wchar_t* VideoEncodeThreadCommandStatusName(
    VideoEncodeThreadCommandStatus status) noexcept {
  switch (status) {
    case VideoEncodeThreadCommandStatus::Accepted:
      return L"accepted";
    case VideoEncodeThreadCommandStatus::AlreadyRunning:
      return L"already running";
    case VideoEncodeThreadCommandStatus::NotRunning:
      return L"not running";
    case VideoEncodeThreadCommandStatus::InvalidConfig:
      return L"invalid config";
    case VideoEncodeThreadCommandStatus::Failed:
      return L"failed";
  }
  return L"unknown";
}

}  // namespace olouie::encode
