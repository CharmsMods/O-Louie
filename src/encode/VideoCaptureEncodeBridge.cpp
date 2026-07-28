#include "encode/VideoCaptureEncodeBridge.h"

#include <string>
#include <utility>

namespace olouie::encode {
namespace {

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

}  // namespace

VideoCaptureEncodeBridge::VideoCaptureEncodeBridge(
    VideoEncodeChain* chain,
    VideoCaptureEncodeBridgeOptions options)
    : chain_(chain), options_(options) {}

bool VideoCaptureEncodeBridge::IsConfigured() const noexcept {
  return chain_ != nullptr && chain_->IsConfigured() &&
         (!options_.start_timebase_on_first_frame ||
          options_.timestamp_frequency > 0);
}

capture::ICapturedVideoFrameSink*
VideoCaptureEncodeBridge::captured_frame_sink() noexcept {
  return this;
}

capture::CapturedVideoFrameSinkResult
VideoCaptureEncodeBridge::OnCapturedVideoFrame(
    capture::OwnedVideoFrame frame) {
  ++stats_.received_frame_count;

  if (!frame.IsValid()) {
    ++stats_.invalid_frame_count;
    return Result(capture::CapturedVideoFrameSinkStatus::InvalidFrame,
                  L"Captured video frame is invalid.");
  }

  if (!IsConfigured()) {
    ++stats_.invalid_config_count;
    return Result(capture::CapturedVideoFrameSinkStatus::InvalidConfig,
                  L"Video capture encode bridge needs a configured chain.");
  }

  auto timebase_result = EnsureFrameTimebase(frame.timestamp_ticks);
  if (!timebase_result.Succeeded()) {
    return timebase_result;
  }

  last_queue_result_ = chain_->QueueFrame(std::move(frame));
  const auto status = SinkStatusFromQueue(last_queue_result_.status);
  auto result =
      Result(status, L"", last_queue_result_.dropped_frame_count);
  if (result.Accepted()) {
    ++stats_.queued_frame_count;
  }
  if (result.Dropped()) {
    stats_.dropped_frame_count +=
        result.dropped_frame_count == 0 ? 1 : result.dropped_frame_count;
  }

  switch (status) {
    case capture::CapturedVideoFrameSinkStatus::Success:
    case capture::CapturedVideoFrameSinkStatus::Dropped:
    case capture::CapturedVideoFrameSinkStatus::DroppedAndQueued:
      return result;
    case capture::CapturedVideoFrameSinkStatus::InvalidFrame:
      ++stats_.invalid_frame_count;
      result.message = L"Video encode chain rejected an invalid frame.";
      return result;
    case capture::CapturedVideoFrameSinkStatus::InvalidConfig:
      ++stats_.invalid_config_count;
      result.message = L"Video encode chain queue is not configured.";
      return result;
    case capture::CapturedVideoFrameSinkStatus::SinkError:
      break;
  }

  ++stats_.sink_error_count;
  return Result(capture::CapturedVideoFrameSinkStatus::SinkError,
                L"Video encode chain queue failed unexpectedly.");
}

VideoEncodeWorkerResult VideoCaptureEncodeBridge::DrainQueuedFrames() {
  if (chain_ == nullptr) {
    VideoEncodeWorkerResult result;
    result.status = VideoEncodeWorkerStatus::InvalidArgument;
    result.message = L"Video capture encode bridge needs a video chain.";
    return result;
  }

  return chain_->DrainQueuedFrames();
}

VideoEncodeWorkerResult VideoCaptureEncodeBridge::DrainQueuedFrames(
    size_t max_frames) {
  if (chain_ == nullptr) {
    VideoEncodeWorkerResult result;
    result.status = VideoEncodeWorkerStatus::InvalidArgument;
    result.message = L"Video capture encode bridge needs a video chain.";
    return result;
  }

  return chain_->DrainQueuedFrames(max_frames);
}

VideoEncodeWorkerResult VideoCaptureEncodeBridge::DrainAllQueuedFrames() {
  if (chain_ == nullptr) {
    VideoEncodeWorkerResult result;
    result.status = VideoEncodeWorkerStatus::InvalidArgument;
    result.message = L"Video capture encode bridge needs a video chain.";
    return result;
  }

  return chain_->DrainAllQueuedFrames();
}

const VideoCaptureEncodeBridgeOptions&
VideoCaptureEncodeBridge::options() const noexcept {
  return options_;
}

const VideoCaptureEncodeBridgeStats&
VideoCaptureEncodeBridge::stats() const noexcept {
  return stats_;
}

const capture::VideoFrameQueuePushResult&
VideoCaptureEncodeBridge::last_queue_result() const noexcept {
  return last_queue_result_;
}

int64_t VideoCaptureEncodeBridge::first_timestamp_ticks() const noexcept {
  return first_timestamp_ticks_;
}

capture::CapturedVideoFrameSinkResult
VideoCaptureEncodeBridge::EnsureFrameTimebase(int64_t timestamp_ticks) {
  if (!options_.start_timebase_on_first_frame || timebase_started_) {
    return Result(capture::CapturedVideoFrameSinkStatus::Success, L"");
  }

  std::wstring error;
  auto timebase = record::Timebase::FromQpc(options_.timestamp_frequency,
                                            timestamp_ticks, &error);
  if (!timebase.IsValid()) {
    ++stats_.invalid_config_count;
    return Result(capture::CapturedVideoFrameSinkStatus::InvalidConfig,
                  std::move(error));
  }

  auto worker_options = chain_->config().worker_options;
  worker_options.timebase = timebase;
  chain_->SetWorkerOptions(worker_options);
  timebase_started_ = true;
  first_timestamp_ticks_ = timestamp_ticks;
  ++stats_.timebase_start_count;
  return Result(capture::CapturedVideoFrameSinkStatus::Success, L"");
}

capture::CapturedVideoFrameSinkResult VideoCaptureEncodeBridge::Result(
    capture::CapturedVideoFrameSinkStatus status,
    std::wstring message,
    uint32_t dropped_frame_count) const {
  capture::CapturedVideoFrameSinkResult result;
  result.status = status;
  result.message = std::move(message);
  result.dropped_frame_count = dropped_frame_count;
  return result;
}

}  // namespace olouie::encode
