#include "encode/VideoEncodeChain.h"

#include <utility>

namespace olouie::encode {
namespace {

BgraVideoRecordingSessionResult Result(VideoRecordingSessionStatus status,
                                       std::wstring message) {
  BgraVideoRecordingSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

}  // namespace

bool VideoEncodeChainConfig::IsValid() const noexcept {
  return queue_options.capacity > 0 && session_options.video_track_id > 0 &&
         session_options.drain_timeout_ms > 0 &&
         session_options.source_width > 0 &&
         session_options.source_height > 0 &&
         worker_options.timebase.IsValid() &&
         worker_options.fallback_frame_duration_ns > 0 &&
         drain_frame_budget > 0;
}

VideoEncodeChain::VideoEncodeChain(VideoEncodeChainConfig config)
    : config_(std::move(config)),
      queue_(config_.queue_options),
      session_(config_.session_options),
      worker_(&queue_, &session_, config_.worker_options) {}

BgraVideoRecordingSessionResult VideoEncodeChain::Prepare(
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    ID3D11DeviceContext* d3d_context,
    record::PacketStore* packet_store) {
  std::lock_guard lock(processing_mutex_);
  if (!config_.IsValid()) {
    prepare_result_ = Result(VideoRecordingSessionStatus::InvalidConfig,
                             L"Video encode chain needs valid queue, BGRA "
                             L"session, worker timing, and drain budget "
                             L"configuration.");
    return prepare_result_;
  }

  prepare_result_ =
      session_.Prepare(encoder_session, d3d_device, d3d_context, packet_store);
  RefreshRuntimeSnapshotLocked();
  return prepare_result_;
}

capture::VideoFrameQueuePushResult VideoEncodeChain::QueueFrame(
    capture::OwnedVideoFrame frame) {
  return queue_.Push(std::move(frame));
}

VideoEncodeWorkerResult VideoEncodeChain::DrainQueuedFrames() {
  std::lock_guard lock(processing_mutex_);
  auto result = worker_.DrainQueuedFrames(config_.drain_frame_budget);
  RefreshRuntimeSnapshotLocked();
  return result;
}

VideoEncodeWorkerResult VideoEncodeChain::DrainQueuedFrames(
    size_t max_frames) {
  std::lock_guard lock(processing_mutex_);
  auto result = worker_.DrainQueuedFrames(max_frames);
  RefreshRuntimeSnapshotLocked();
  return result;
}

VideoEncodeWorkerResult VideoEncodeChain::DrainAllQueuedFrames() {
  std::lock_guard lock(processing_mutex_);
  auto result = worker_.DrainAllQueuedFrames();
  RefreshRuntimeSnapshotLocked();
  return result;
}

BgraVideoRecordingSessionResult VideoEncodeChain::FinalizeEncoder() {
  std::lock_guard lock(processing_mutex_);
  auto result = session_.Finalize();
  RefreshRuntimeSnapshotLocked();
  return result;
}

void VideoEncodeChain::SetWorkerOptions(VideoEncodeWorkerOptions options) {
  std::lock_guard lock(processing_mutex_);
  config_.worker_options = std::move(options);
  worker_.SetOptions(config_.worker_options);
  RefreshRuntimeSnapshotLocked();
}

void VideoEncodeChain::ClearQueuedFrames() {
  queue_.Clear();
}

bool VideoEncodeChain::IsConfigured() const noexcept {
  return config_.IsValid();
}

bool VideoEncodeChain::IsPrepared() const noexcept {
  std::lock_guard lock(processing_mutex_);
  return session_.IsPrepared();
}

uint32_t VideoEncodeChain::queued_frame_count() const {
  return queue_.Size();
}

bool VideoEncodeChain::queue_empty() const {
  return queue_.Empty();
}

const VideoEncodeChainConfig& VideoEncodeChain::config() const noexcept {
  return config_;
}

const BgraVideoRecordingSessionResult& VideoEncodeChain::prepare_result()
    const noexcept {
  return prepare_result_;
}

capture::VideoFrameQueueStats VideoEncodeChain::queue_stats() const {
  return queue_.stats();
}

const VideoEncodeWorkerStats& VideoEncodeChain::worker_stats()
    const noexcept {
  return worker_.stats();
}

const BgraVideoRecordingSessionStats& VideoEncodeChain::session_stats()
    const noexcept {
  return session_.stats();
}

const H264PacketStoreConfig& VideoEncodeChain::session_config()
    const noexcept {
  return session_.config();
}

const graphics::GpuBgraToNv12Plan& VideoEncodeChain::conversion_plan()
    const noexcept {
  return session_.conversion_plan();
}

const graphics::GpuBgraToNv12ConverterStats&
VideoEncodeChain::converter_stats() const noexcept {
  return session_.converter_stats();
}

VideoEncodeChainRuntimeSnapshot VideoEncodeChain::SnapshotRuntime() const {
  VideoEncodeChainRuntimeSnapshot snapshot;
  {
    std::lock_guard lock(runtime_snapshot_mutex_);
    snapshot = runtime_snapshot_;
  }
  snapshot.queue = queue_.stats();
  return snapshot;
}

void VideoEncodeChain::RefreshRuntimeSnapshotLocked() {
  VideoEncodeChainRuntimeSnapshot snapshot;
  snapshot.worker = worker_.stats();
  snapshot.session = session_.stats();
  snapshot.h264 = session_.config();
  snapshot.conversion_plan = session_.conversion_plan();
  snapshot.converter = session_.converter_stats();
  snapshot.encoder = session_.encoder_info_snapshot();
  std::lock_guard lock(runtime_snapshot_mutex_);
  runtime_snapshot_ = std::move(snapshot);
}

}  // namespace olouie::encode
