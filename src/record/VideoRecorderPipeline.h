#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/AudioRecordingSession.h"
#include "capture/BgraTexturePool.h"
#include "capture/VideoFrameQueue.h"
#include "encode/VideoRecordingBootstrap.h"
#include "graphics/GpuBgraToNv12.h"
#include "performance/MultimediaThreadScheduling.h"
#include "record/DiskWriteFault.h"
#include "record/VideoRecorderClip.h"

namespace olouie::record {

struct VideoRecorderAudioTrackRuntime {
  uint32_t track_id = 0;
  std::wstring name;
  bool packet_bearing = false;
};

struct VideoRecorderRuntimeFacts {
  std::wstring monitor_device_name;
  std::wstring encoder_name;
  uint32_t requested_fps_numerator = 0;
  uint32_t requested_fps_denominator = 0;
  uint32_t requested_bitrate_bps = 0;
  uint32_t negotiated_fps_numerator = 0;
  uint32_t negotiated_fps_denominator = 0;
  uint32_t negotiated_bitrate_bps = 0;
  uint64_t recording_elapsed_ns = 0;
  uint64_t encoded_video_payload_bytes = 0;
  uint32_t queued_video_frame_count = 0;
  uint32_t peak_queued_video_frame_count = 0;
  size_t video_queue_capacity = 0;
  uint64_t video_queue_oldest_frame_age_ns = 0;
  uint64_t video_queue_maximum_frame_age_ns = 0;
  uint64_t video_queue_overflow_event_count = 0;
  uint64_t video_queue_backlog_recovery_count = 0;
  uint64_t video_queue_dropped_newest_count = 0;
  uint64_t video_queue_dropped_oldest_count = 0;
  uint64_t video_queue_dropped_backlog_count = 0;
  capture::VideoFrameQueueOverflowReason video_queue_last_overflow_reason =
      capture::VideoFrameQueueOverflowReason::None;
  uint64_t video_texture_pool_exhausted_frame_count = 0;
  capture::BgraTexturePoolStats video_texture_pool;
  graphics::GpuBgraToNv12ConverterStats video_converter;
  uint64_t video_capture_copy_submission_count = 0;
  uint64_t video_capture_copy_last_latency_ns = 0;
  uint64_t video_capture_copy_maximum_latency_ns = 0;
  uint64_t video_capture_copy_total_latency_ns = 0;
  uint64_t video_queue_wait_sample_count = 0;
  uint64_t video_queue_wait_last_ns = 0;
  uint64_t video_queue_wait_maximum_ns = 0;
  uint64_t video_queue_wait_total_ns = 0;
  uint64_t video_encoder_wait_count = 0;
  uint64_t video_encoder_wait_last_ns = 0;
  uint64_t video_encoder_wait_maximum_ns = 0;
  uint64_t video_encoder_wait_total_ns = 0;
  PacketStoreWriterStats packet_writer;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
  performance::MultimediaThreadSchedulingSnapshot recorder_scheduling;
  performance::MultimediaThreadSchedulingSnapshot capture_scheduling;
  performance::MultimediaThreadSchedulingSnapshot video_encode_scheduling;
  std::vector<encode::MfHardwareH264CodecSettingResult>
      encoder_codec_settings;
  std::vector<performance::MultimediaThreadSchedulingSnapshot>
      audio_capture_scheduling;
  uint32_t process_priority_class = NORMAL_PRIORITY_CLASS;
  bool realtime_process_priority = false;
  size_t queued_audio_block_count = 0;
  size_t peak_queued_audio_block_count = 0;
  size_t audio_queue_capacity_per_track = 0;
  size_t pending_export_command_count = 0;
  uint64_t submitted_export_count = 0;
  uint64_t saved_export_count = 0;
  uint64_t failed_export_count = 0;
  uint64_t rejected_export_count = 0;
  size_t outstanding_export_count = 0;
  bool export_queue_running = false;
  std::vector<VideoRecorderAudioTrackRuntime> audio_tracks;
};

enum class VideoRecorderPipelineStage {
  BackendCheck,
  Prepare,
  CaptureStart,
  Recording,
  CaptureStop,
  QueueDrain,
  EncoderDrain,
  ClipExportDrain,
  ManifestWrite,
  PacketStoreClose,
  PacketStoreRecover,
  ExportPlan,
  Mp4Write,
  Complete,
};

enum class VideoRecorderPipelineStatus {
  Success,
  InvalidConfig,
  Cancelled,
  BackendUnavailable,
  PrepareFailed,
  CaptureStartFailed,
  CaptureFailed,
  CaptureStopFailed,
  QueueDrainFailed,
  EncoderDrainFailed,
  ClipExportDrainFailed,
  ManifestWriteFailed,
  PacketStoreCloseFailed,
  PacketStoreRecoverFailed,
  ExportPlanFailed,
  Mp4WriteFailed,
};

struct VideoRecorderPipelineProgress;
using VideoRecorderPipelineProgressSink =
    std::function<void(const VideoRecorderPipelineProgress&)>;

struct VideoRecorderPipelineOptions {
  HMONITOR monitor = nullptr;
  std::wstring monitor_device_name;
  std::filesystem::path session_root_directory;
  std::filesystem::path output_directory;
  encode::VideoRecordingPreflightOptions preflight;
  encode::MfHardwareH264EncoderProbeOptions encoder_probe_options;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
  bool use_source_output_dimensions = true;
  bool capture_cursor = true;
  std::chrono::milliseconds drain_interval{10};
  std::optional<audio::AudioRecordingSessionOptions> audio;
  std::shared_ptr<VideoRecorderClipCommandQueue> clip_commands;
  VideoRecorderClipEventSink clip_event_sink;
  size_t clip_export_queue_capacity = 2;
  std::chrono::milliseconds diagnostics_interval{1000};
  VideoRecorderPipelineProgressSink progress_sink;
};

struct VideoRecorderPipelineStats {
  uint64_t captured_frame_count = 0;
  uint64_t accepted_frame_count = 0;
  uint64_t rate_limited_frame_count = 0;
  uint64_t dropped_frame_count = 0;
  uint64_t encoded_frame_count = 0;
  uint64_t encoded_packet_count = 0;
  uint64_t discarded_video_frame_count = 0;
  encode::VideoRecordingRuntimeFaultKind video_runtime_fault =
      encode::VideoRecordingRuntimeFaultKind::None;
  uint64_t captured_audio_packet_count = 0;
  uint64_t captured_audio_frame_count = 0;
  uint64_t audio_data_discontinuity_count = 0;
  uint64_t audio_timestamp_error_count = 0;
  uint64_t audio_retimed_packet_count = 0;
  uint64_t audio_endpoint_invalidation_count = 0;
  uint64_t audio_default_device_change_count = 0;
  uint64_t audio_restart_attempt_count = 0;
  uint64_t audio_restart_success_count = 0;
  uint64_t audio_capture_format_change_count = 0;
  uint64_t synthetic_audio_silence_packet_count = 0;
  uint64_t synthetic_audio_silence_frame_count = 0;
  uint64_t encoded_audio_packet_count = 0;
  uint64_t clip_request_count = 0;
  uint64_t clip_saved_count = 0;
  uint64_t clip_failed_count = 0;
  uint64_t bookmark_request_count = 0;
  uint64_t bookmark_count = 0;
  uint64_t bookmark_saved_count = 0;
  uint64_t bookmark_failed_count = 0;
  VideoRecorderRuntimeFacts runtime;
};

struct VideoRecorderPipelineProgress {
  std::filesystem::path session_directory;
  std::filesystem::path output_path;
  VideoRecorderPipelineStats stats;
};

struct VideoRecorderPipelineResult {
  VideoRecorderPipelineStatus status =
      VideoRecorderPipelineStatus::InvalidConfig;
  VideoRecorderPipelineStage failed_stage =
      VideoRecorderPipelineStage::BackendCheck;
  std::wstring message;
  std::filesystem::path session_directory;
  std::filesystem::path output_path;
  VideoRecorderPipelineStats stats;
  bool recording_saved_after_failure = false;
  DiskWriteFault write_fault;

  bool Succeeded() const noexcept;
};

enum class VideoRecorderPipelineFailureDisposition {
  Abort,
  FinalizeRecording,
};

struct VideoRecorderPipelineStepResult {
  bool succeeded = false;
  std::wstring message;
  VideoRecorderPipelineFailureDisposition failure_disposition =
      VideoRecorderPipelineFailureDisposition::Abort;
  DiskWriteFault write_fault;

  static VideoRecorderPipelineStepResult Success();
  static VideoRecorderPipelineStepResult Failure(
      std::wstring message,
      VideoRecorderPipelineFailureDisposition failure_disposition =
          VideoRecorderPipelineFailureDisposition::Abort,
      DiskWriteFault write_fault = {});
};

using VideoRecorderPipelineStageSink =
    std::function<void(VideoRecorderPipelineStage)>;

class IVideoRecorderPipelineBackend {
 public:
  virtual ~IVideoRecorderPipelineBackend() = default;

  virtual VideoRecorderPipelineStepResult CheckBackend() = 0;
  virtual VideoRecorderPipelineStepResult Prepare() = 0;
  virtual VideoRecorderPipelineStepResult StartCapture() = 0;
  virtual VideoRecorderPipelineStepResult DrainCaptureTick() = 0;
  virtual VideoRecorderPipelineStepResult ProcessClipRequests() = 0;
  virtual VideoRecorderPipelineStepResult StopCapture() = 0;
  virtual VideoRecorderPipelineStepResult DrainQueuedFrames() = 0;
  virtual VideoRecorderPipelineStepResult DrainEncoder() = 0;
  virtual VideoRecorderPipelineStepResult DrainClipExports() = 0;
  virtual VideoRecorderPipelineStepResult WriteManifest() = 0;
  virtual VideoRecorderPipelineStepResult ClosePacketStore() = 0;
  virtual VideoRecorderPipelineStepResult RecoverPacketStore() = 0;
  virtual VideoRecorderPipelineStepResult BuildExportPlan() = 0;
  virtual VideoRecorderPipelineStepResult WriteMp4() = 0;

  virtual std::filesystem::path session_directory() const = 0;
  virtual std::filesystem::path output_path() const = 0;
  virtual VideoRecorderPipelineStats stats() const = 0;
};

VideoRecorderPipelineResult RunVideoRecorderPipeline(
    const VideoRecorderPipelineOptions& options,
    std::atomic_bool* stop_requested,
    VideoRecorderPipelineStageSink stage_sink = {});

VideoRecorderPipelineResult RunVideoRecorderPipelineWithBackend(
    const VideoRecorderPipelineOptions& options,
    std::atomic_bool* stop_requested,
    IVideoRecorderPipelineBackend* backend,
    VideoRecorderPipelineStageSink stage_sink = {});

const wchar_t* VideoRecorderPipelineStageName(
    VideoRecorderPipelineStage stage) noexcept;
const wchar_t* VideoRecorderPipelineStatusName(
    VideoRecorderPipelineStatus status) noexcept;

}  // namespace olouie::record
