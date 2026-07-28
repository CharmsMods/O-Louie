#include "encode/VideoLiveCaptureEncode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace olouie::encode {
namespace {

constexpr std::chrono::milliseconds kMaxLiveVideoCaptureEncodeDuration{30000};
constexpr std::chrono::milliseconds kMaxDrainInterval{1000};

VideoLiveCaptureEncodeRunResult Result(VideoLiveCaptureEncodeStatus status,
                                       std::wstring message) {
  VideoLiveCaptureEncodeRunResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool OptionsAreValid(const VideoLiveCaptureEncodeOptions& options) noexcept {
  return options.duration > std::chrono::milliseconds(0) &&
         options.duration <= kMaxLiveVideoCaptureEncodeDuration &&
         options.drain_interval > std::chrono::milliseconds(0) &&
         options.drain_interval <= kMaxDrainInterval &&
         options.max_copied_frames > 0 &&
         options.max_frames_per_drain_tick > 0 &&
         options.timestamp_frequency > 0 &&
         !(options.start_timebase_on_first_frame &&
           options.use_explicit_timebase_origin) &&
         (!options.use_explicit_timebase_origin ||
          options.timebase_origin_ticks >= 0);
}

capture::WgcFrameCopySmokeResult DefaultWgcVideoFrameCopyRunner(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames,
    capture::ICapturedVideoFrameSink* sink) {
  return capture::RunWgcMonitorFrameCopySmoke(
      monitor, device, context, duration, max_copied_frames, sink);
}

class SerializedVideoFrameSink final
    : public capture::ICapturedVideoFrameSink {
 public:
  SerializedVideoFrameSink(VideoCaptureEncodeBridge* bridge,
                           std::mutex* mutex)
      : bridge_(bridge), mutex_(mutex) {}

  capture::CapturedVideoFrameSinkResult OnCapturedVideoFrame(
      capture::OwnedVideoFrame frame) override {
    std::lock_guard lock(*mutex_);
    return bridge_->captured_frame_sink()->OnCapturedVideoFrame(
        std::move(frame));
  }

 private:
  VideoCaptureEncodeBridge* bridge_ = nullptr;
  std::mutex* mutex_ = nullptr;
};

}  // namespace

bool VideoLiveCaptureEncodeRunResult::Succeeded() const noexcept {
  return status == VideoLiveCaptureEncodeStatus::Success;
}

VideoLiveCaptureEncodeRunResult RunWgcVideoLiveCaptureEncode(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const VideoLiveCaptureEncodeOptions& options,
    VideoEncodeChain* chain,
    VideoLiveCaptureEncodeResult* result) {
  return RunWgcVideoLiveCaptureEncode(
      monitor, device, context, options, chain,
      &DefaultWgcVideoFrameCopyRunner, result);
}

VideoLiveCaptureEncodeRunResult RunWgcVideoLiveCaptureEncode(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const VideoLiveCaptureEncodeOptions& options,
    VideoEncodeChain* chain,
    WgcVideoFrameCopyRunner capture_runner,
    VideoLiveCaptureEncodeResult* result) {
  if (result == nullptr) {
    return Result(VideoLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live video capture encode needs an output destination.");
  }

  *result = {};

  if (!OptionsAreValid(options)) {
    return Result(VideoLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live video capture encode options are invalid.");
  }

  if (chain == nullptr || !chain->IsConfigured()) {
    return Result(VideoLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live video capture encode needs a configured video chain.");
  }

  if (capture_runner == nullptr) {
    return Result(VideoLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live video capture encode needs a capture runner.");
  }

  VideoCaptureEncodeBridge bridge(
      chain, VideoCaptureEncodeBridgeOptions{
                 options.timestamp_frequency,
                 options.start_timebase_on_first_frame});
  result->bridge_configured = bridge.IsConfigured();
  if (!bridge.IsConfigured()) {
    return Result(VideoLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live video capture encode needs a configured bridge.");
  }

  std::mutex bridge_mutex;
  SerializedVideoFrameSink serialized_sink(&bridge, &bridge_mutex);
  std::atomic_bool capture_done{false};
  capture::WgcFrameCopySmokeResult capture_result;

  std::thread capture_thread([&] {
    capture_result = capture_runner(monitor, device, context, options.duration,
                                    options.max_copied_frames,
                                    &serialized_sink);
    capture_done.store(true);
  });

  bool tick_drain_failed = false;
  std::wstring first_failure;
  const auto deadline = std::chrono::steady_clock::now() + options.duration;

  auto drain_tick = [&]() {
    std::lock_guard lock(bridge_mutex);
    result->last_tick_drain =
        bridge.DrainQueuedFrames(options.max_frames_per_drain_tick);
    if (result->last_tick_drain.Succeeded()) {
      ++result->drain_tick_count;
    }
    result->drained_frame_count +=
        result->last_tick_drain.processed_frame_count;
    return result->last_tick_drain;
  };

  while (!capture_done.load() && std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = deadline - now;
    const auto sleep_duration =
        std::min(options.drain_interval,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     remaining));
    if (sleep_duration > std::chrono::milliseconds(0)) {
      std::this_thread::sleep_for(sleep_duration);
    }

    const auto drained = drain_tick();
    if (!drained.Succeeded()) {
      tick_drain_failed = true;
      if (first_failure.empty()) {
        first_failure = drained.message;
      }
      break;
    }
  }

  if (capture_thread.joinable()) {
    capture_thread.join();
  }

  {
    std::lock_guard lock(bridge_mutex);
    result->final_drain = bridge.DrainAllQueuedFrames();
    result->drained_frame_count +=
        result->final_drain.processed_frame_count;
    result->bridge_stats = bridge.stats();
    result->queue_stats = chain->queue_stats();
    result->session_stats = chain->session_stats();
    result->first_timestamp_ticks = bridge.first_timestamp_ticks();
  }
  result->capture = std::move(capture_result);

  if (!result->final_drain.Succeeded()) {
    return Result(VideoLiveCaptureEncodeStatus::DrainFailed,
                  result->final_drain.message);
  }

  if (tick_drain_failed) {
    return Result(VideoLiveCaptureEncodeStatus::DrainFailed,
                  std::move(first_failure));
  }

  if (!result->capture.error.empty()) {
    return Result(VideoLiveCaptureEncodeStatus::CaptureFailed,
                  result->capture.error);
  }

  return Result(VideoLiveCaptureEncodeStatus::Success, L"");
}

const wchar_t* VideoLiveCaptureEncodeStatusName(
    VideoLiveCaptureEncodeStatus status) noexcept {
  switch (status) {
    case VideoLiveCaptureEncodeStatus::Success:
      return L"success";
    case VideoLiveCaptureEncodeStatus::InvalidConfig:
      return L"invalid config";
    case VideoLiveCaptureEncodeStatus::CaptureFailed:
      return L"capture failed";
    case VideoLiveCaptureEncodeStatus::DrainFailed:
      return L"drain failed";
  }

  return L"unknown";
}

}  // namespace olouie::encode
