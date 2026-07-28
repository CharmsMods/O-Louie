#include "record/VideoRecorderPipeline.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "audio/AudioLiveCaptureEncode.h"
#include "audio/AudioRecordingMetadata.h"
#include "capture/WgcMonitorCaptureSession.h"
#include "encode/VideoEncodeThread.h"
#include "encode/VideoRecordingMetadata.h"
#include "graphics/DisplayManager.h"
#include "product/ProductIdentity.h"
#include "record/ActiveRecordingBookmark.h"
#include "record/ActiveRecordingClip.h"
#include "record/ClipExportQueue.h"
#include "record/Mp4Muxer.h"
#include "record/SessionClock.h"
#include "record/SessionManifest.h"
#include "record/Timebase.h"
#include "record/VideoExportPlan.h"

namespace olouie::record {
namespace {

constexpr std::chrono::milliseconds kMaximumDrainInterval{1000};

uint64_t TimestampTicksToNanoseconds(uint64_t ticks,
                                    int64_t frequency) noexcept {
  if (frequency <= 0 || ticks == 0) {
    return 0;
  }
  const long double nanoseconds =
      static_cast<long double>(ticks) * 1000000000.0L /
      static_cast<long double>(frequency);
  if (nanoseconds >=
      static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(nanoseconds);
}

VideoRecorderPipelineResult Result(VideoRecorderPipelineStatus status,
                                   VideoRecorderPipelineStage failed_stage,
                                   std::wstring message,
                                   DiskWriteFault write_fault = {}) {
  VideoRecorderPipelineResult result;
  result.status = status;
  result.failed_stage = failed_stage;
  result.message = std::move(message);
  result.write_fault = std::move(write_fault);
  return result;
}

void EmitStage(const VideoRecorderPipelineStageSink& sink,
               VideoRecorderPipelineStage stage) {
  if (sink) {
    sink(stage);
  }
}

VideoRecorderPipelineStatus FailureStatusForStage(
    VideoRecorderPipelineStage stage) noexcept {
  switch (stage) {
    case VideoRecorderPipelineStage::BackendCheck:
      return VideoRecorderPipelineStatus::BackendUnavailable;
    case VideoRecorderPipelineStage::Prepare:
      return VideoRecorderPipelineStatus::PrepareFailed;
    case VideoRecorderPipelineStage::CaptureStart:
      return VideoRecorderPipelineStatus::CaptureStartFailed;
    case VideoRecorderPipelineStage::Recording:
      return VideoRecorderPipelineStatus::CaptureFailed;
    case VideoRecorderPipelineStage::CaptureStop:
      return VideoRecorderPipelineStatus::CaptureStopFailed;
    case VideoRecorderPipelineStage::QueueDrain:
      return VideoRecorderPipelineStatus::QueueDrainFailed;
    case VideoRecorderPipelineStage::EncoderDrain:
      return VideoRecorderPipelineStatus::EncoderDrainFailed;
    case VideoRecorderPipelineStage::ClipExportDrain:
      return VideoRecorderPipelineStatus::ClipExportDrainFailed;
    case VideoRecorderPipelineStage::ManifestWrite:
      return VideoRecorderPipelineStatus::ManifestWriteFailed;
    case VideoRecorderPipelineStage::PacketStoreClose:
      return VideoRecorderPipelineStatus::PacketStoreCloseFailed;
    case VideoRecorderPipelineStage::PacketStoreRecover:
      return VideoRecorderPipelineStatus::PacketStoreRecoverFailed;
    case VideoRecorderPipelineStage::ExportPlan:
      return VideoRecorderPipelineStatus::ExportPlanFailed;
    case VideoRecorderPipelineStage::Mp4Write:
      return VideoRecorderPipelineStatus::Mp4WriteFailed;
    case VideoRecorderPipelineStage::Complete:
      return VideoRecorderPipelineStatus::Success;
  }
  return VideoRecorderPipelineStatus::InvalidConfig;
}

const graphics::MonitorInfo* ResolveMonitor(
    const std::vector<graphics::MonitorInfo>& monitors,
    HMONITOR requested, std::wstring_view requested_device_name) {
  if (!requested_device_name.empty()) {
    for (const auto& monitor : monitors) {
      if (monitor.device_name == requested_device_name) {
        return &monitor;
      }
    }
    return nullptr;
  }
  if (requested == nullptr) {
    return graphics::FindPrimaryMonitor(monitors);
  }
  for (const auto& monitor : monitors) {
    if (monitor.handle == requested) {
      return &monitor;
    }
  }
  return nullptr;
}

std::wstring NewRecordingStem() {
  static std::atomic_uint32_t sequence{0};
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t buffer[96]{};
  swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u-%03u-p%lu-%u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds, GetCurrentProcessId(),
             sequence.fetch_add(1));
  return buffer;
}

class ProductionVideoRecorderPipelineBackend final
    : public IVideoRecorderPipelineBackend {
 public:
  explicit ProductionVideoRecorderPipelineBackend(
      VideoRecorderPipelineOptions options)
      : options_(std::move(options)) {}

  ~ProductionVideoRecorderPipelineBackend() override {
    (void)StopCapture();
    if (video_encode_thread_ != nullptr) {
      (void)video_encode_thread_->StopAndDrain();
    }
    if (audio_live_ != nullptr) {
      (void)audio_live_->DrainQueuedBlocks();
      (void)audio_live_->FlushEncoders();
      audio_live_.reset();
    }
    (void)DrainClipExports();
    (void)ClosePacketStore();
    video_encode_thread_.reset();
    audio_recording_.reset();
    bootstrap_.Reset();
    recovered_ = PacketStore{};
    if (com_initialized_) {
      CoUninitialize();
    }
  }

  VideoRecorderPipelineStepResult CheckBackend() override {
    const auto availability = Mp4Muxer::BackendAvailability();
    if (!availability.Available()) {
      return VideoRecorderPipelineStepResult::Failure(
          availability.message.empty()
              ? L"The FFmpeg MP4 backend is not configured."
              : availability.message);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult Prepare() override {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Recorder worker COM initialization failed.");
    }
    com_initialized_ = true;

    std::error_code error;
    std::filesystem::create_directories(options_.session_root_directory,
                                        error);
    if (error) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Could not create the recording session directory root.");
    }
    std::filesystem::create_directories(options_.output_directory, error);
    if (error) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Could not create the recording output directory.");
    }

    recording_stem_ = NewRecordingStem();
    session_directory_ =
        options_.session_root_directory / (L"recording-" + recording_stem_);
    output_path_ = options_.output_directory /
                   (product::kRecordingFilePrefix + recording_stem_ + L".mp4");

    if (options_.audio.has_value()) {
      audio_recording_ = std::make_unique<audio::AudioRecordingSession>(
          *options_.audio);
      const auto audio_preflight = audio_recording_->Preflight();
      if (!audio_preflight.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            audio_preflight.message.empty()
                ? L"Audio endpoint/format preflight failed."
                : audio_preflight.message);
      }
    }

    const auto monitors = graphics::EnumerateMonitors();
    const auto* selected_monitor = ResolveMonitor(
        monitors, options_.monitor, options_.monitor_device_name);
    if (selected_monitor == nullptr || selected_monitor->Width() < 2 ||
        selected_monitor->Height() < 2) {
      return VideoRecorderPipelineStepResult::Failure(
          L"The selected monitor is unavailable.");
    }

    auto bootstrap_options = encode::VideoRecordingBootstrapOptions{};
    bootstrap_options.monitor = selected_monitor->handle;
    bootstrap_options.packet_store_session_dir = session_directory_;
    bootstrap_options.preflight = options_.preflight;
    bootstrap_options.encoder_probe_options = options_.encoder_probe_options;
    if (audio_recording_ != nullptr) {
      bootstrap_options.additional_packet_tracks =
          audio_recording_->preflight().plan.packet_tracks;
    }

    if (options_.use_source_output_dimensions) {
      bootstrap_options.preflight.encoder_config.width =
          static_cast<uint32_t>(selected_monitor->Width()) & ~uint32_t{1};
      bootstrap_options.preflight.encoder_config.height =
          static_cast<uint32_t>(selected_monitor->Height()) & ~uint32_t{1};
    }

    const auto bootstrap_result =
        encode::BuildVideoRecordingBootstrapSession(bootstrap_options,
                                                    &bootstrap_);
    if (!bootstrap_result.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(
          bootstrap_result.message.empty()
              ? std::wstring(encode::VideoRecordingBootstrapStatusName(
                    bootstrap_result.status))
              : bootstrap_result.message);
    }

    auto* recording = bootstrap_.recording_session.get();
    if (audio_recording_ != nullptr) {
      const auto audio_prepare =
          audio_recording_->Prepare(&bootstrap_.packet_store);
      if (!audio_prepare.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            audio_prepare.message.empty()
                ? L"AAC encoder/session preparation failed."
                : audio_prepare.message);
      }
    }

    std::wstring clock_error;
    if (!CaptureSessionClock(&session_clock_, &clock_error)) {
      return VideoRecorderPipelineStepResult::Failure(
          clock_error.empty() ? L"Could not establish the recording clock."
                              : std::move(clock_error));
    }
    auto worker_options = recording->encode_chain()->config().worker_options;
    worker_options.timebase = Timebase::FromQpc(
        kSystemRelativeTimestampFrequency, session_clock_.origin_100ns,
        &clock_error);
    if (!worker_options.timebase.IsValid()) {
      return VideoRecorderPipelineStepResult::Failure(
          clock_error.empty() ? L"Could not configure the video session clock."
                              : std::move(clock_error));
    }
    recording->encode_chain()->SetWorkerOptions(worker_options);

    encode::VideoEncodeThreadOptions video_thread_options;
    video_thread_options.max_frames_per_batch =
        options_.preflight.live.max_frames_per_drain_tick;
    video_thread_options.performance_mode = options_.performance_mode;
    video_encode_thread_ = std::make_unique<encode::VideoEncodeThread>(
        recording->encode_chain(), video_thread_options);
    if (!recording->encode_chain()->IsConfigured()) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Could not configure the event-driven video encode worker.");
    }

    if (audio_recording_ != nullptr) {
      auto live_options = options_.audio->live;
      live_options.duration = std::chrono::milliseconds(0);
      live_options.qpc_origin_ns = session_clock_.origin_ns;
      audio_live_ = std::make_unique<audio::AudioLiveCaptureEncodeSession>(
          audio_recording_->preflight().plan, live_options,
          audio_recording_->encode_session(),
          options_.audio->live_source_factory);
      const auto live_prepare = audio_live_->Prepare();
      if (!live_prepare.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            live_prepare.message.empty()
                ? L"Could not prepare live audio capture."
                : live_prepare.message);
      }
    }

    if (options_.clip_commands != nullptr) {
      ClipExportQueueOptions queue_options;
      queue_options.capacity = options_.clip_export_queue_capacity;
      clip_export_queue_ =
          std::make_unique<ClipExportQueue>(std::move(queue_options));
      const auto started = clip_export_queue_->Start(
          [this](const ClipExportCompletion& completion) {
            HandleClipCompletion(completion);
          });
      if (!started.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            started.message.empty()
                ? L"Could not start the clip export queue."
                : started.message);
      }
    }

    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult StartCapture() override {
    if (!bootstrap_.IsPrepared() || video_encode_thread_ == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Recorder pipeline is not prepared for capture.");
    }

    if (audio_live_ != nullptr) {
      const auto audio_started = audio_live_->Start();
      if (!audio_started.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            audio_started.message.empty()
                ? L"Requested audio capture could not start."
                : audio_started.message);
      }
    }

    const auto video_started = video_encode_thread_->Start();
    if (!video_started.Accepted()) {
      if (audio_live_ != nullptr) {
        (void)audio_live_->StopSources();
        (void)audio_live_->DrainQueuedBlocks();
        (void)audio_live_->FlushEncoders();
      }
      return VideoRecorderPipelineStepResult::Failure(
          video_started.message.empty()
              ? L"The event-driven video encode worker could not start."
              : video_started.message);
    }

    capture::WgcMonitorCaptureSessionOptions capture_options;
    capture_options.capture_cursor = options_.capture_cursor;
    capture_options.performance_mode = options_.performance_mode;
    capture_options.frame_cadence = capture::VideoFrameCadenceConfig{
        kSystemRelativeTimestampFrequency,
        options_.preflight.encoder_config.fps_numerator,
        options_.preflight.encoder_config.fps_denominator};
    capture_options.owned_texture_pool_capacity =
        options_.preflight.queue_capacity >
                std::numeric_limits<uint32_t>::max() - 2u
            ? std::numeric_limits<uint32_t>::max()
            : options_.preflight.queue_capacity + 2u;
    const auto result = capture_.Start(
        bootstrap_.monitor.handle, bootstrap_.d3d.device(),
        bootstrap_.d3d.immediate_context(), capture_options,
        video_encode_thread_->captured_frame_sink());
    if (!result.Succeeded()) {
      std::wstring message = result.message;
      const auto video_stopped = video_encode_thread_->StopAndDrain();
      if (video_stopped.status ==
              encode::VideoEncodeThreadCommandStatus::Failed &&
          !video_stopped.message.empty()) {
        message += L" Video cleanup: " + video_stopped.message;
      }
      if (audio_live_ != nullptr) {
        const auto stopped = audio_live_->StopSources();
        const auto drained = audio_live_->DrainQueuedBlocks();
        const auto flushed = audio_live_->FlushEncoders();
        for (const auto* cleanup : {&stopped, &drained, &flushed}) {
          if (!cleanup->Succeeded() && !cleanup->message.empty()) {
            message += L" Audio cleanup: " + cleanup->message;
          }
        }
      }
      return VideoRecorderPipelineStepResult::Failure(std::move(message));
    }
    recording_started_at_ = std::chrono::steady_clock::now();
    recording_stopped_at_ = {};
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult DrainCaptureTick() override {
    const auto capture_snapshot = capture_.Snapshot();
    last_capture_snapshot_ = capture_snapshot;
    if (capture_snapshot.Failed()) {
      const bool selected_monitor_changed =
          capture::IsSelectedMonitorTopologyFault(
              capture_snapshot.fault.kind);
      const bool d3d11_device_lost = capture::IsWgcD3D11DeviceFault(
          capture_snapshot.fault.kind);
      if (d3d11_device_lost) {
        LatchVideoRuntimeFailure(
            encode::VideoRecordingRuntimeFaultKind::D3D11DeviceLost,
            capture_snapshot.error);
      }
      return VideoRecorderPipelineStepResult::Failure(
          capture_snapshot.error,
          selected_monitor_changed || d3d11_device_lost
              ? VideoRecorderPipelineFailureDisposition::FinalizeRecording
              : VideoRecorderPipelineFailureDisposition::Abort);
    }
    if (video_encode_thread_ == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Video encode worker is unavailable.");
    }
    const auto video_snapshot = video_encode_thread_->Snapshot();
    if (video_snapshot.Failed()) {
      if (video_snapshot.runtime_fault !=
          encode::VideoRecordingRuntimeFaultKind::None) {
        LatchVideoRuntimeFailure(video_snapshot.runtime_fault,
                                 video_snapshot.message);
        return VideoRecorderPipelineStepResult::Failure(
            video_runtime_failure_message_,
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
      return FailureForPossiblePacketStoreWrite(
          video_snapshot.message.empty()
              ? L"The event-driven video encode worker failed."
              : video_snapshot.message,
          VideoRecorderPipelineFailureDisposition::FinalizeRecording);
    }
    const auto async_write_fault =
        bootstrap_.packet_store.last_write_fault();
    if (async_write_fault.Failed()) {
      if (!packet_store_write_fault_.Failed()) {
        packet_store_write_fault_ = async_write_fault;
      }
      return VideoRecorderPipelineStepResult::Failure(
          DescribeDiskWriteFault(packet_store_write_fault_),
          VideoRecorderPipelineFailureDisposition::FinalizeRecording,
          packet_store_write_fault_);
    }
    if (audio_live_ != nullptr) {
      UpdateAudioQueueDepthPeak();
      const auto audio_drained = audio_live_->DrainTick();
      UpdateAudioQueueDepthPeak();
      if (!audio_drained.Succeeded()) {
        return FailureForPossiblePacketStoreWrite(
            audio_drained.message.empty()
                ? L"Live audio capture or AAC draining failed."
                : audio_drained.message,
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult ProcessClipRequests() override {
    if (options_.clip_commands == nullptr) {
      return VideoRecorderPipelineStepResult::Success();
    }

    VideoRecorderClipRequest request;
    while (options_.clip_commands->TryPop(&request)) {
      if (request.kind == VideoRecorderExportKind::Bookmark) {
        ++bookmark_request_count_;
      } else {
        ++clip_request_count_;
      }
      const std::wstring export_kind =
          request.kind == VideoRecorderExportKind::Bookmark
              ? L"bookmark"
              : L"clip";
      auto final_output =
          options_.output_directory /
          (product::kRecordingFilePrefix + recording_stem_ + L"-" +
           export_kind + L"-" +
           std::to_wstring(request.request_id) + L".mp4");
      auto temp_output = final_output;
      temp_output.replace_extension(L".partial.mp4");

      if (request.duration.count() >
          std::numeric_limits<int64_t>::max() / 1000000) {
        PublishExportFailure(request, final_output,
                             L"Export duration is too large.");
        continue;
      }

      SessionManifest live_manifest;
      const auto metadata = BuildCurrentManifest(&live_manifest);
      if (!metadata.succeeded) {
        PublishExportFailure(request, final_output, metadata.message);
        continue;
      }

      ActiveRecordingClipPlan clip_plan;
      if (request.kind == VideoRecorderExportKind::Bookmark) {
        if (request.bookmark_pre_roll.count() >
                std::numeric_limits<int64_t>::max() / 1000000 ||
            request.bookmark_post_roll.count() >
                std::numeric_limits<int64_t>::max() / 1000000) {
          PublishExportFailure(request, final_output,
                               L"Bookmark export duration is too large.");
          continue;
        }

        ActiveRecordingBookmarkOptions bookmark_options;
        bookmark_options.pre_roll_ns =
            request.bookmark_pre_roll.count() * 1000000;
        bookmark_options.post_roll_ns =
            request.bookmark_post_roll.count() * 1000000;
        bookmark_options.temp_output_path = temp_output;
        bookmark_options.final_output_path = final_output;

        ActiveRecordingBookmarkPlan bookmark_plan;
        const auto planned = BuildActiveRecordingBookmarkPlan(
            live_manifest, &bootstrap_.packet_store, &bookmarks_,
            bookmark_options, &bookmark_plan);
        if (!planned.Succeeded()) {
          PublishExportFailure(
              request, final_output,
              planned.message.empty()
                  ? L"Could not plan the active recording bookmark."
                  : planned.message);
          if (bootstrap_.packet_store.last_write_fault().Failed()) {
            return FailureForPossiblePacketStoreWrite(planned.message);
          }
          continue;
        }
        request.bookmark_id = bookmark_plan.bookmark.id;
        request.bookmark_time_ns = bookmark_plan.bookmark.time_ns;
        clip_plan = std::move(bookmark_plan.clip_plan);
        final_output =
            options_.output_directory /
            (product::kRecordingFilePrefix + recording_stem_ + L"-bookmark-" +
             std::to_wstring(request.bookmark_id) + L".mp4");
        temp_output = final_output;
        temp_output.replace_extension(L".partial.mp4");
        clip_plan.mux_request.temp_output_path = temp_output;
        clip_plan.mux_request.final_output_path = final_output;
      } else {
        ActiveRecordingClipOptions clip_options;
        clip_options.duration_ns = request.duration.count() * 1000000;
        clip_options.temp_output_path = temp_output;
        clip_options.final_output_path = final_output;

        const auto planned = BuildActiveRecordingClipPlan(
            live_manifest, &bootstrap_.packet_store, clip_options,
            &clip_plan);
        if (!planned.Succeeded()) {
          PublishExportFailure(
              request, final_output,
              planned.message.empty()
                  ? L"Could not plan the active recording clip."
                  : planned.message);
          if (bootstrap_.packet_store.last_write_fault().Failed()) {
            return FailureForPossiblePacketStoreWrite(planned.message);
          }
          continue;
        }
      }

      if (clip_export_queue_ == nullptr) {
        PublishExportFailure(request, final_output,
                             L"Clip export queue is unavailable.");
        continue;
      }

      ClipExportJob job;
      job.kind = request.kind;
      job.request_id = request.request_id;
      job.duration = request.duration;
      job.bookmark_id = request.bookmark_id;
      job.bookmark_time_ns = request.bookmark_time_ns;
      job.output_path = final_output;
      job.mux_request = std::move(clip_plan.mux_request);
      const auto queued = clip_export_queue_->Enqueue(std::move(job));
      if (!queued.Succeeded()) {
        PublishExportFailure(
            request, final_output,
            queued.message.empty() ? L"Could not queue the clip export."
                                   : queued.message);
      } else if (request.kind == VideoRecorderExportKind::Bookmark) {
        PublishBookmarkQueued(request, final_output);
      }
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult StopCapture() override {
    std::wstring message;
    if (recording_started_at_ != std::chrono::steady_clock::time_point{} &&
        recording_stopped_at_ == std::chrono::steady_clock::time_point{}) {
      recording_stopped_at_ = std::chrono::steady_clock::now();
    }
    if (capture_.IsRunning()) {
      const auto result = capture_.Stop();
      last_capture_snapshot_ = capture_.Snapshot();
      if (!result.Succeeded() &&
          video_runtime_fault_ ==
              encode::VideoRecordingRuntimeFaultKind::None) {
        message = result.message;
      }
    }
    if (audio_live_ != nullptr) {
      const auto audio_stopped = audio_live_->StopSources();
      if (!audio_stopped.Succeeded()) {
        if (!message.empty()) {
          message += L" ";
        }
        message += audio_stopped.message;
      }
    }
    if (!message.empty()) {
      return VideoRecorderPipelineStepResult::Failure(
          std::move(message),
          VideoRecorderPipelineFailureDisposition::FinalizeRecording);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult DrainQueuedFrames() override {
    if (video_encode_thread_ == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Video encode worker is unavailable for final draining.");
    }
    if (packet_store_write_fault_.Failed()) {
      (void)video_encode_thread_->StopAndDrain();
      DiscardQueuedVideoFrames();
      return VideoRecorderPipelineStepResult::Success();
    }
    auto video_result = VideoRecorderPipelineStepResult::Success();
    if (video_runtime_fault_ !=
        encode::VideoRecordingRuntimeFaultKind::None) {
      (void)video_encode_thread_->StopAndDrain();
      DiscardQueuedVideoFrames();
    } else {
      const auto stopped = video_encode_thread_->StopAndDrain();
      if (!stopped.Accepted()) {
        const auto snapshot = video_encode_thread_->Snapshot();
        const auto runtime_fault = snapshot.runtime_fault;
        if (runtime_fault ==
            encode::VideoRecordingRuntimeFaultKind::None) {
          return FailureForPossiblePacketStoreWrite(
              snapshot.message.empty()
                  ? L"The event-driven video encode worker could not drain."
                  : snapshot.message);
        }
        LatchVideoRuntimeFailure(runtime_fault, snapshot.message);
        DiscardQueuedVideoFrames();
        video_result = VideoRecorderPipelineStepResult::Failure(
            video_runtime_failure_message_,
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
    }
    if (audio_live_ != nullptr) {
      const auto audio_drained = audio_live_->DrainQueuedBlocks();
      if (!audio_drained.Succeeded()) {
        std::wstring message = audio_drained.message.empty()
                                   ? L"Prepared PCM could not be fully drained."
                                   : audio_drained.message;
        if (!video_result.succeeded && !video_result.message.empty()) {
          message = video_result.message + L" Audio finalization failed: " +
                    message;
        }
        return FailureForPossiblePacketStoreWrite(
            std::move(message),
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
    }
    return video_result;
  }

  VideoRecorderPipelineStepResult DrainEncoder() override {
    if (!bootstrap_.IsPrepared() ||
        bootstrap_.recording_session->encode_chain() == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Video encoder chain is unavailable for final draining.");
    }
    if (packet_store_write_fault_.Failed()) {
      return VideoRecorderPipelineStepResult::Success();
    }
    auto video_result = VideoRecorderPipelineStepResult::Success();
    if (video_runtime_fault_ ==
        encode::VideoRecordingRuntimeFaultKind::None) {
      const auto drained =
          bootstrap_.recording_session->encode_chain()->FinalizeEncoder();
      if (!drained.Succeeded()) {
        const auto runtime_fault = drained.RuntimeFaultKind();
        if (runtime_fault ==
            encode::VideoRecordingRuntimeFaultKind::None) {
          return FailureForPossiblePacketStoreWrite(drained.message);
        }
        LatchVideoRuntimeFailure(runtime_fault, drained.message);
        video_result = VideoRecorderPipelineStepResult::Failure(
            video_runtime_failure_message_,
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
    }
    if (audio_live_ != nullptr) {
      const auto audio_flushed = audio_live_->FlushEncoders();
      if (!audio_flushed.Succeeded()) {
        std::wstring message = audio_flushed.message.empty()
                                   ? L"AAC encoders could not be flushed."
                                   : audio_flushed.message;
        if (!video_result.succeeded && !video_result.message.empty()) {
          message = video_result.message + L" Audio finalization failed: " +
                    message;
        }
        return FailureForPossiblePacketStoreWrite(
            std::move(message),
            VideoRecorderPipelineFailureDisposition::FinalizeRecording);
      }
    }
    return video_result;
  }

  VideoRecorderPipelineStepResult DrainClipExports() override {
    const auto processed = ProcessClipRequests();
    if (clip_export_queue_ != nullptr) {
      clip_export_queue_->Shutdown();
    }
    if (!processed.succeeded) {
      return processed;
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult WriteManifest() override {
    const auto built = BuildCurrentManifest(&manifest_);
    if (!built.succeeded) {
      return built;
    }
    const auto write = WriteSessionManifest(manifest_);
    if (!write.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(
          write.message, VideoRecorderPipelineFailureDisposition::Abort,
          write.write_fault);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult ClosePacketStore() override {
    std::wstring error;
    if (!bootstrap_.packet_store.Close(&error)) {
      const auto fault = bootstrap_.packet_store.last_write_fault();
      if (packet_store_write_fault_.Failed()) {
        return VideoRecorderPipelineStepResult::Success();
      }
      return VideoRecorderPipelineStepResult::Failure(
          error.empty() ? DescribeDiskWriteFault(fault) : std::move(error),
          VideoRecorderPipelineFailureDisposition::Abort, fault);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult RecoverPacketStore() override {
    if (manifest_.session_dir.empty()) {
      const auto read = ReadSessionManifest(session_directory_, &manifest_);
      if (!read.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(read.message);
      }
    }
    std::wstring error;
    recovered_ = PacketStore::Recover(session_directory_, &error);
    if (!error.empty() || recovered_.session_dir().empty()) {
      return VideoRecorderPipelineStepResult::Failure(
          error.empty() ? L"PacketStore recovery did not produce a session."
                        : std::move(error));
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult BuildExportPlan() override {
    if (manifest_.video.track_id == 0 || recovered_.session_dir().empty()) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Recovered video metadata is unavailable for export planning.");
    }

    int64_t recording_end_ns = 0;
    for (const auto& packet : recovered_.SnapshotIndex()) {
      if (packet.metadata.track_id == manifest_.video.track_id &&
          packet.metadata.codec_id == CodecId::H264) {
        recording_end_ns = std::max(recording_end_ns, packet.EndPtsNs());
      }
    }
    if (recording_end_ns <= 0) {
      return VideoRecorderPipelineStepResult::Failure(
          L"The recording does not contain an exportable H.264 packet.");
    }

    VideoExportPlanOptions export_options;
    export_options.requested_start_ns = 0;
    export_options.requested_end_ns = recording_end_ns;
    export_options.include_previous_keyframe = true;
    export_options.require_keyframe_start = true;
    export_options.normalize_timestamps = true;
    export_options.require_all_audio_tracks = true;
    const auto built = BuildVideoExportPlan(
        manifest_, &recovered_, export_options, &export_plan_);
    if (!built.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(built.message);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  VideoRecorderPipelineStepResult WriteMp4() override {
    auto temp_output_path = output_path_;
    temp_output_path.replace_extension(L".partial.mp4");
    const auto built = BuildVideoMp4MuxRequest(
        export_plan_, temp_output_path, output_path_, false, &mux_request_);
    if (!built.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(built.message);
    }
    const auto written = Mp4Muxer{}.WriteMp4(mux_request_);
    if (!written.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(
          written.message, VideoRecorderPipelineFailureDisposition::Abort,
          written.write_fault);
    }
    return VideoRecorderPipelineStepResult::Success();
  }

  std::filesystem::path session_directory() const override {
    return session_directory_;
  }

  std::filesystem::path output_path() const override {
    return output_path_;
  }

  VideoRecorderPipelineStats stats() const override {
    VideoRecorderPipelineStats stats;
    stats.captured_frame_count = last_capture_snapshot_.received_frame_count;
    stats.accepted_frame_count = last_capture_snapshot_.accepted_frame_count;
    stats.rate_limited_frame_count =
        last_capture_snapshot_.rate_limited_frame_count;
    stats.dropped_frame_count = last_capture_snapshot_.dropped_frame_count;
    if (bootstrap_.recording_session != nullptr &&
        bootstrap_.recording_session->encode_chain() != nullptr) {
      const auto* chain = bootstrap_.recording_session->encode_chain();
      const auto chain_runtime = chain->SnapshotRuntime();
      stats.encoded_frame_count =
          chain_runtime.session.submitted_frame_count;
      stats.encoded_packet_count =
          chain_runtime.session.appended_packet_count;
      const auto& queue = chain_runtime.queue;
      auto& runtime = stats.runtime;
      runtime.queued_video_frame_count = queue.current_depth;
      runtime.peak_queued_video_frame_count = queue.peak_depth;
      runtime.video_queue_capacity = chain->config().queue_options.capacity;
      runtime.video_queue_oldest_frame_age_ns = TimestampTicksToNanoseconds(
          queue.current_oldest_frame_age_ticks,
          chain->config().worker_options.timebase.qpc_frequency());
      runtime.video_queue_maximum_frame_age_ns = TimestampTicksToNanoseconds(
          queue.maximum_oldest_frame_age_ticks,
          chain->config().worker_options.timebase.qpc_frequency());
      runtime.video_queue_overflow_event_count = queue.overflow_event_count;
      runtime.video_queue_backlog_recovery_count =
          queue.backlog_recovery_count;
      runtime.video_queue_dropped_newest_count = queue.dropped_newest_count;
      runtime.video_queue_dropped_oldest_count = queue.dropped_oldest_count;
      runtime.video_queue_dropped_backlog_count = queue.dropped_backlog_count;
      runtime.video_queue_last_overflow_reason = queue.last_overflow_reason;
      runtime.video_converter = chain_runtime.converter;
      runtime.video_queue_wait_sample_count =
          chain_runtime.worker.queue_wait_sample_count;
      runtime.video_queue_wait_last_ns =
          chain_runtime.worker.last_queue_wait_ns;
      runtime.video_queue_wait_maximum_ns =
          chain_runtime.worker.maximum_queue_wait_ns;
      runtime.video_queue_wait_total_ns =
          chain_runtime.worker.total_queue_wait_ns;
      runtime.video_encoder_wait_count =
          chain_runtime.session.encoder_wait_count;
      runtime.video_encoder_wait_last_ns =
          chain_runtime.session.last_encoder_wait_ns;
      runtime.video_encoder_wait_maximum_ns =
          chain_runtime.session.maximum_encoder_wait_ns;
      runtime.video_encoder_wait_total_ns =
          chain_runtime.session.total_encoder_wait_ns;
    }
    stats.runtime.video_texture_pool_exhausted_frame_count =
        last_capture_snapshot_.texture_pool_exhausted_frame_count;
    stats.runtime.video_texture_pool = last_capture_snapshot_.texture_pool;
    stats.runtime.video_capture_copy_submission_count =
        last_capture_snapshot_.texture_copy_submission_count;
    stats.runtime.video_capture_copy_last_latency_ns =
        last_capture_snapshot_.last_texture_copy_submission_latency_ns;
    stats.runtime.video_capture_copy_maximum_latency_ns =
        last_capture_snapshot_.maximum_texture_copy_submission_latency_ns;
    stats.runtime.video_capture_copy_total_latency_ns =
        last_capture_snapshot_.total_texture_copy_submission_latency_ns;
    stats.discarded_video_frame_count = discarded_video_frame_count_;
    stats.video_runtime_fault = video_runtime_fault_;
    if (audio_live_ != nullptr) {
      const auto& audio_result = audio_live_->result();
      stats.captured_audio_packet_count = audio_result.packet_count;
      stats.captured_audio_frame_count = audio_result.frame_count;
      for (const auto& source : audio_result.sources) {
        stats.runtime.audio_capture_scheduling.push_back(
            source.capture.scheduling);
        stats.audio_data_discontinuity_count +=
            source.capture.data_discontinuity_count;
        stats.audio_timestamp_error_count +=
            source.capture.timestamp_error_count;
        stats.audio_retimed_packet_count += source.retimed_packet_count;
        stats.audio_endpoint_invalidation_count +=
            source.capture.endpoint_invalidation_count;
        stats.audio_default_device_change_count +=
            source.capture.default_device_change_count;
        stats.audio_restart_attempt_count +=
            source.capture.restart_attempt_count;
        stats.audio_restart_success_count +=
            source.capture.restart_success_count;
        stats.audio_capture_format_change_count +=
            source.capture.capture_format_change_count;
        stats.synthetic_audio_silence_packet_count +=
            source.synthetic_silence_packet_count;
        stats.synthetic_audio_silence_frame_count +=
            source.synthetic_silence_frame_count;
      }
    }
    PacketStoreStats packet_stats;
    if (bootstrap_.packet_store.IsWritable()) {
      packet_stats = bootstrap_.packet_store.SnapshotStats();
    } else if (!recovered_.session_dir().empty()) {
      packet_stats = recovered_.SnapshotStats();
    }
    stats.runtime.packet_writer =
        bootstrap_.packet_store.SnapshotWriterStats();
    for (const auto& track : packet_stats.tracks) {
      if (track.codec_id == CodecId::Aac) {
        stats.encoded_audio_packet_count += track.packet_count;
      } else if (track.codec_id == CodecId::H264) {
        stats.runtime.encoded_video_payload_bytes +=
            track.payload_byte_count;
      }
    }
    stats.clip_request_count = clip_request_count_.load();
    stats.clip_saved_count = clip_saved_count_.load();
    stats.clip_failed_count = clip_failed_count_.load();
    stats.bookmark_request_count = bookmark_request_count_.load();
    stats.bookmark_count = bookmarks_.Count();
    stats.bookmark_saved_count = bookmark_saved_count_.load();
    stats.bookmark_failed_count = bookmark_failed_count_.load();

    auto& runtime = stats.runtime;
    runtime.performance_mode = options_.performance_mode;
    runtime.capture_scheduling = last_capture_snapshot_.scheduling;
    if (video_encode_thread_ != nullptr) {
      runtime.video_encode_scheduling =
          video_encode_thread_->Snapshot().scheduling;
    }
    if (audio_recording_ != nullptr &&
        audio_recording_->encode_session() != nullptr) {
      runtime.queued_audio_block_count =
          audio_recording_->encode_session()->TotalQueuedBlockCount();
      runtime.peak_queued_audio_block_count = peak_queued_audio_block_count_;
      runtime.audio_queue_capacity_per_track =
          options_.audio->setup.queue_capacity;
    }
    runtime.requested_fps_numerator =
        options_.preflight.encoder_config.fps_numerator;
    runtime.requested_fps_denominator =
        options_.preflight.encoder_config.fps_denominator;
    runtime.requested_bitrate_bps =
        options_.preflight.encoder_config.bitrate_bps;
    if (bootstrap_.IsPrepared()) {
      runtime.monitor_device_name = bootstrap_.monitor.device_name;
      const auto& encoder = bootstrap_.encoder_session->info();
      runtime.encoder_name = encoder.encoder.name;
      runtime.negotiated_fps_numerator = encoder.media_type.fps_numerator;
      runtime.negotiated_fps_denominator = encoder.media_type.fps_denominator;
      runtime.negotiated_bitrate_bps = encoder.media_type.bitrate_bps;
      runtime.encoder_codec_settings = encoder.codec_settings;
    }
    if (recording_started_at_ != std::chrono::steady_clock::time_point{}) {
      const auto end =
          recording_stopped_at_ == std::chrono::steady_clock::time_point{}
              ? std::chrono::steady_clock::now()
              : recording_stopped_at_;
      runtime.recording_elapsed_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              end - recording_started_at_)
              .count());
    }
    if (options_.clip_commands != nullptr) {
      runtime.pending_export_command_count =
          options_.clip_commands->pending_count();
    }
    if (clip_export_queue_ != nullptr) {
      const auto queue = clip_export_queue_->Snapshot();
      runtime.submitted_export_count = queue.submitted_job_count;
      runtime.saved_export_count = queue.saved_job_count;
      runtime.failed_export_count = queue.failed_job_count;
      runtime.rejected_export_count = queue.rejected_job_count;
      runtime.outstanding_export_count = queue.outstanding_job_count;
      runtime.export_queue_running = queue.running;
    }
    if (audio_recording_ != nullptr) {
      for (const auto& track : audio_recording_->preflight().plan.tracks) {
        VideoRecorderAudioTrackRuntime audio_track;
        audio_track.track_id = track.track_id;
        audio_track.name = track.name;
        audio_track.packet_bearing = std::any_of(
            packet_stats.tracks.begin(), packet_stats.tracks.end(),
            [&track](const PacketStoreTrackStats& packet_track) {
              return packet_track.track_id == track.track_id &&
                     packet_track.codec_id == CodecId::Aac &&
                     packet_track.packet_count != 0;
            });
        runtime.audio_tracks.push_back(std::move(audio_track));
      }
    }
    return stats;
  }

 private:
  VideoRecorderPipelineStepResult FailureForPossiblePacketStoreWrite(
      std::wstring fallback_message,
      VideoRecorderPipelineFailureDisposition fallback_disposition =
          VideoRecorderPipelineFailureDisposition::Abort) {
    const auto fault = bootstrap_.packet_store.last_write_fault();
    if (!fault.Failed()) {
      return VideoRecorderPipelineStepResult::Failure(
          std::move(fallback_message), fallback_disposition);
    }
    if (!packet_store_write_fault_.Failed()) {
      packet_store_write_fault_ = fault;
    }
    return VideoRecorderPipelineStepResult::Failure(
        DescribeDiskWriteFault(packet_store_write_fault_),
        VideoRecorderPipelineFailureDisposition::FinalizeRecording,
        packet_store_write_fault_);
  }

  void UpdateAudioQueueDepthPeak() {
    if (audio_recording_ == nullptr ||
        audio_recording_->encode_session() == nullptr) {
      return;
    }
    peak_queued_audio_block_count_ = std::max(
        peak_queued_audio_block_count_,
        audio_recording_->encode_session()->TotalQueuedBlockCount());
  }

  void LatchVideoRuntimeFailure(
      encode::VideoRecordingRuntimeFaultKind fault,
      std::wstring message) {
    if (fault == encode::VideoRecordingRuntimeFaultKind::None ||
        video_runtime_fault_ !=
            encode::VideoRecordingRuntimeFaultKind::None) {
      return;
    }

    video_runtime_fault_ = fault;
    if (message.empty()) {
      message = fault ==
                        encode::VideoRecordingRuntimeFaultKind::D3D11DeviceLost
                    ? L"The D3D11 recording device was lost."
                    : L"The hardware H.264 encoder failed.";
    } else if (fault ==
               encode::VideoRecordingRuntimeFaultKind::HardwareEncoderFailed) {
      message = L"Hardware H.264 encoder failed: " + message;
    }
    video_runtime_failure_message_ = std::move(message);
  }

  void DiscardQueuedVideoFrames() {
    if (bootstrap_.recording_session == nullptr ||
        bootstrap_.recording_session->encode_chain() == nullptr) {
      return;
    }
    auto* chain = bootstrap_.recording_session->encode_chain();
    discarded_video_frame_count_ += chain->queued_frame_count();
    chain->ClearQueuedFrames();
  }

  VideoRecorderPipelineStepResult BuildCurrentManifest(
      SessionManifest* manifest) {
    if (manifest == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Session metadata needs an output destination.");
    }

    if (!bootstrap_.IsPrepared() ||
        bootstrap_.recording_session->encode_chain() == nullptr) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Video recording metadata needs a prepared encode chain.");
    }
    const auto runtime =
        bootstrap_.recording_session->encode_chain()->SnapshotRuntime();
    encode::VideoRecordingMetadataInputs metadata_inputs;
    metadata_inputs.packet_store_session_dir =
        bootstrap_.packet_store.session_dir();
    metadata_inputs.packet_file_path =
        bootstrap_.packet_store.packet_file_path();
    metadata_inputs.video_track =
        bootstrap_.recording_session->preflight().video_track;
    metadata_inputs.requested_config =
        bootstrap_.recording_session->preflight().encoder_config;
    metadata_inputs.h264 = runtime.h264;
    metadata_inputs.encoder_info = runtime.encoder;
    metadata_inputs.monitor = bootstrap_.monitor;
    metadata_inputs.conversion_plan = runtime.conversion_plan;

    encode::VideoRecordingMetadata metadata;
    const auto metadata_result = encode::BuildVideoRecordingMetadata(
        metadata_inputs, &metadata);
    if (!metadata_result.Succeeded()) {
      return VideoRecorderPipelineStepResult::Failure(
          metadata_result.message.empty()
              ? L"H.264 session metadata is not ready."
              : metadata_result.message);
    }

    auto built = encode::BuildVideoRecordingSessionManifest(metadata);
    if (audio_recording_ != nullptr) {
      const auto audio_metadata = audio::BuildAudioRecordingMetadata(
          audio_recording_->preflight(), audio_recording_->setup(),
          &built.audio_tracks);
      if (!audio_metadata.Succeeded()) {
        return VideoRecorderPipelineStepResult::Failure(
            audio_metadata.message.empty()
                ? L"Could not build AAC session metadata."
                : audio_metadata.message);
      }
    }
    built.bookmarks = bookmarks_.Snapshot();
    if (!built.IsReady()) {
      return VideoRecorderPipelineStepResult::Failure(
          L"Current recording metadata is incomplete for clip export.");
    }
    *manifest = std::move(built);
    return VideoRecorderPipelineStepResult::Success();
  }

  void PublishExportFailure(const VideoRecorderClipRequest& request,
                            const std::filesystem::path& output_path,
                            std::wstring message) {
    if (request.kind == VideoRecorderExportKind::Bookmark) {
      ++bookmark_failed_count_;
    } else {
      ++clip_failed_count_;
    }
    if (options_.clip_event_sink) {
      VideoRecorderClipEvent event;
      event.kind = request.kind;
      event.state = VideoRecorderClipState::Failed;
      event.request_id = request.request_id;
      event.duration = request.duration;
      event.bookmark_id = request.bookmark_id;
      event.bookmark_time_ns = request.bookmark_time_ns;
      event.output_path = output_path;
      event.message = message.empty() ? L"Recording export failed."
                                      : std::move(message);
      options_.clip_event_sink(event);
    }
  }

  void PublishBookmarkQueued(
      const VideoRecorderClipRequest& request,
      const std::filesystem::path& output_path) {
    if (!options_.clip_event_sink) {
      return;
    }
    VideoRecorderClipEvent event;
    event.kind = VideoRecorderExportKind::Bookmark;
    event.state = VideoRecorderClipState::Queued;
    event.request_id = request.request_id;
    event.duration = request.duration;
    event.bookmark_id = request.bookmark_id;
    event.bookmark_time_ns = request.bookmark_time_ns;
    event.output_path = output_path;
    event.message = L"Bookmark added; export queued.";
    options_.clip_event_sink(event);
  }

  void HandleClipCompletion(const ClipExportCompletion& completion) {
    VideoRecorderClipEvent event;
    event.kind = completion.job.kind;
    event.request_id = completion.job.request_id;
    event.duration = completion.job.duration;
    event.bookmark_id = completion.job.bookmark_id;
    event.bookmark_time_ns = completion.job.bookmark_time_ns;
    event.output_path = completion.job.output_path;
    if (completion.result.Succeeded()) {
      if (completion.job.kind == VideoRecorderExportKind::Bookmark) {
        ++bookmark_saved_count_;
      } else {
        ++clip_saved_count_;
      }
      event.state = VideoRecorderClipState::Saved;
      event.message = completion.job.kind == VideoRecorderExportKind::Bookmark
                          ? L"Bookmark clip saved."
                          : L"Clip saved.";
    } else {
      if (completion.job.kind == VideoRecorderExportKind::Bookmark) {
        ++bookmark_failed_count_;
      } else {
        ++clip_failed_count_;
      }
      event.state = VideoRecorderClipState::Failed;
      event.message = completion.result.message.empty()
                          ? L"Recording export failed."
                          : completion.result.message;
    }
    if (options_.clip_event_sink) {
      options_.clip_event_sink(event);
    }
  }

  VideoRecorderPipelineOptions options_;
  bool com_initialized_ = false;
  encode::VideoRecordingBootstrapSession bootstrap_;
  std::unique_ptr<encode::VideoEncodeThread> video_encode_thread_;
  std::unique_ptr<audio::AudioRecordingSession> audio_recording_;
  std::unique_ptr<audio::AudioLiveCaptureEncodeSession> audio_live_;
  std::unique_ptr<ClipExportQueue> clip_export_queue_;
  BookmarkCollection bookmarks_;
  SessionClock session_clock_;
  capture::WgcMonitorCaptureSession capture_;
  capture::WgcMonitorCaptureSessionSnapshot last_capture_snapshot_;
  std::chrono::steady_clock::time_point recording_started_at_;
  std::chrono::steady_clock::time_point recording_stopped_at_;
  std::filesystem::path session_directory_;
  std::filesystem::path output_path_;
  std::wstring recording_stem_;
  SessionManifest manifest_;
  PacketStore recovered_;
  VideoExportPlan export_plan_;
  Mp4MuxRequest mux_request_;
  encode::VideoRecordingRuntimeFaultKind video_runtime_fault_ =
      encode::VideoRecordingRuntimeFaultKind::None;
  std::wstring video_runtime_failure_message_;
  DiskWriteFault packet_store_write_fault_;
  uint64_t discarded_video_frame_count_ = 0;
  size_t peak_queued_audio_block_count_ = 0;
  std::atomic_uint64_t clip_request_count_{0};
  std::atomic_uint64_t clip_saved_count_{0};
  std::atomic_uint64_t clip_failed_count_{0};
  std::atomic_uint64_t bookmark_request_count_{0};
  std::atomic_uint64_t bookmark_saved_count_{0};
  std::atomic_uint64_t bookmark_failed_count_{0};
};

}  // namespace

bool VideoRecorderPipelineResult::Succeeded() const noexcept {
  return status == VideoRecorderPipelineStatus::Success;
}

VideoRecorderPipelineStepResult VideoRecorderPipelineStepResult::Success() {
  return {true, L"", VideoRecorderPipelineFailureDisposition::Abort, {}};
}

VideoRecorderPipelineStepResult VideoRecorderPipelineStepResult::Failure(
    std::wstring message,
    VideoRecorderPipelineFailureDisposition failure_disposition,
    DiskWriteFault write_fault) {
  return {false, std::move(message), failure_disposition,
          std::move(write_fault)};
}

VideoRecorderPipelineResult RunVideoRecorderPipeline(
    const VideoRecorderPipelineOptions& options,
    std::atomic_bool* stop_requested,
    VideoRecorderPipelineStageSink stage_sink) {
  ProductionVideoRecorderPipelineBackend backend(options);
  return RunVideoRecorderPipelineWithBackend(
      options, stop_requested, &backend, std::move(stage_sink));
}

VideoRecorderPipelineResult RunVideoRecorderPipelineWithBackend(
    const VideoRecorderPipelineOptions& options,
    std::atomic_bool* stop_requested,
    IVideoRecorderPipelineBackend* backend,
    VideoRecorderPipelineStageSink stage_sink) {
  if (stop_requested == nullptr || backend == nullptr ||
      !performance::IsValidCapturePerformanceMode(
          options.performance_mode) ||
      options.performance_mode !=
          options.preflight.encoder_config.performance_mode ||
      options.drain_interval <= std::chrono::milliseconds(0) ||
      options.drain_interval > kMaximumDrainInterval ||
      (options.progress_sink &&
       options.diagnostics_interval <= std::chrono::milliseconds(0)) ||
      (options.clip_commands != nullptr &&
       options.clip_export_queue_capacity == 0)) {
    return Result(VideoRecorderPipelineStatus::InvalidConfig,
                  VideoRecorderPipelineStage::BackendCheck,
                  L"Video recorder pipeline configuration is invalid.");
  }

  performance::MultimediaThreadRegistration recorder_registration;
  const auto recorder_scheduling = recorder_registration.Register(
      performance::BuildMultimediaThreadSchedulingPlan(
          options.performance_mode,
          performance::MultimediaThreadWorkload::Recorder));

  const auto decorate_stats = [&](VideoRecorderPipelineStats stats) {
    auto& runtime = stats.runtime;
    runtime.performance_mode = options.performance_mode;
    runtime.recorder_scheduling = recorder_scheduling;
    const DWORD priority_class = GetPriorityClass(GetCurrentProcess());
    if (priority_class != 0) {
      runtime.process_priority_class = priority_class;
    }
    runtime.realtime_process_priority =
        runtime.process_priority_class == REALTIME_PRIORITY_CLASS;
    return stats;
  };

  bool prepare_attempted = false;
  bool capture_start_attempted = false;
  bool capture_started = false;
  bool capture_stopped = false;
  bool queues_drained = false;
  bool encoders_drained = false;
  bool clip_exports_drained = false;
  bool packet_store_closed = false;
  bool recording_failure_pending = false;
  VideoRecorderPipelineStage recording_failure_stage =
      VideoRecorderPipelineStage::Recording;
  std::wstring recording_failure_message;
  DiskWriteFault recording_failure_fault;
  DiskWriteFault cleanup_write_fault;

  const auto remember_preservation_failure =
      [&](VideoRecorderPipelineStage stage,
          const VideoRecorderPipelineStepResult& failure) {
        if (recording_failure_pending) {
          return;
        }
        recording_failure_pending = true;
        recording_failure_stage = stage;
        recording_failure_message = failure.message;
        recording_failure_fault = failure.write_fault;
      };

  const auto finish = [&](VideoRecorderPipelineResult result) {
    result.session_directory = backend->session_directory();
    result.output_path = backend->output_path();
    result.stats = decorate_stats(backend->stats());
    return result;
  };

  const auto cleanup = [&]() {
    std::wstring cleanup_message;
    if (capture_start_attempted && !capture_stopped) {
      EmitStage(stage_sink, VideoRecorderPipelineStage::CaptureStop);
      const auto stopped = backend->StopCapture();
      capture_stopped = true;
      if (!stopped.succeeded) {
        cleanup_message = stopped.message;
        if (!cleanup_write_fault.Failed() && stopped.write_fault.Failed()) {
          cleanup_write_fault = stopped.write_fault;
        }
      }
    }
    if (capture_started && !queues_drained) {
      EmitStage(stage_sink, VideoRecorderPipelineStage::QueueDrain);
      const auto drained = backend->DrainQueuedFrames();
      queues_drained = true;
      if (!drained.succeeded) {
        if (!cleanup_write_fault.Failed() && drained.write_fault.Failed()) {
          cleanup_write_fault = drained.write_fault;
        }
        if (!cleanup_message.empty()) {
          cleanup_message += L" ";
        }
        cleanup_message += drained.message;
      }
    }
    if (capture_started && !encoders_drained) {
      EmitStage(stage_sink, VideoRecorderPipelineStage::EncoderDrain);
      const auto drained = backend->DrainEncoder();
      encoders_drained = true;
      if (!drained.succeeded) {
        if (!cleanup_write_fault.Failed() && drained.write_fault.Failed()) {
          cleanup_write_fault = drained.write_fault;
        }
        if (!cleanup_message.empty()) {
          cleanup_message += L" ";
        }
        cleanup_message += drained.message;
      }
    }
    if (prepare_attempted && !clip_exports_drained) {
      EmitStage(stage_sink, VideoRecorderPipelineStage::ClipExportDrain);
      const auto drained = backend->DrainClipExports();
      clip_exports_drained = true;
      if (!drained.succeeded) {
        if (!cleanup_write_fault.Failed() && drained.write_fault.Failed()) {
          cleanup_write_fault = drained.write_fault;
        }
        if (!cleanup_message.empty()) {
          cleanup_message += L" ";
        }
        cleanup_message += drained.message;
      }
    }
    if (prepare_attempted && !packet_store_closed) {
      EmitStage(stage_sink, VideoRecorderPipelineStage::PacketStoreClose);
      const auto closed = backend->ClosePacketStore();
      packet_store_closed = true;
      if (!closed.succeeded) {
        if (!cleanup_write_fault.Failed() && closed.write_fault.Failed()) {
          cleanup_write_fault = closed.write_fault;
        }
        if (!cleanup_message.empty()) {
          cleanup_message += L" ";
        }
        cleanup_message += closed.message;
      }
    }
    return cleanup_message;
  };

  const auto fail = [&](VideoRecorderPipelineStage stage,
                         std::wstring message,
                         DiskWriteFault write_fault = {}) {
    if (recording_failure_pending) {
      std::wstring combined = recording_failure_message;
      if (!message.empty()) {
        if (!combined.empty()) {
          combined += L" Finalization then failed: ";
        }
        combined += message;
      }
      message = std::move(combined);
    }
    const auto cleanup_message = cleanup();
    if (!cleanup_message.empty()) {
      if (!message.empty()) {
        message += L" Cleanup: ";
      }
      message += cleanup_message;
    }
    if (recording_failure_fault.Failed()) {
      write_fault = recording_failure_fault;
    } else if (!write_fault.Failed() && cleanup_write_fault.Failed()) {
      write_fault = cleanup_write_fault;
    }
    return finish(Result(FailureStatusForStage(stage), stage,
                         std::move(message), std::move(write_fault)));
  };

  EmitStage(stage_sink, VideoRecorderPipelineStage::BackendCheck);
  auto step = backend->CheckBackend();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::BackendCheck,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::Prepare);
  prepare_attempted = true;
  step = backend->Prepare();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::Prepare, std::move(step.message),
                step.write_fault);
  }

  if (stop_requested->load()) {
    (void)cleanup();
    return finish(Result(VideoRecorderPipelineStatus::Cancelled,
                         VideoRecorderPipelineStage::Prepare,
                         L"Recording was stopped before capture began."));
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::CaptureStart);
  capture_start_attempted = true;
  step = backend->StartCapture();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::CaptureStart,
                std::move(step.message), step.write_fault);
  }
  capture_started = true;
  EmitStage(stage_sink, VideoRecorderPipelineStage::Recording);

  const auto publish_progress = [&]() {
    if (!options.progress_sink) {
      return;
    }
    VideoRecorderPipelineProgress progress;
    progress.session_directory = backend->session_directory();
    progress.output_path = backend->output_path();
    progress.stats = decorate_stats(backend->stats());
    options.progress_sink(progress);
  };
  publish_progress();
  auto next_diagnostics_publish =
      std::chrono::steady_clock::now() + options.diagnostics_interval;

  while (!stop_requested->load()) {
    step = backend->DrainCaptureTick();
    if (!step.succeeded) {
      if (step.failure_disposition !=
          VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
        return fail(VideoRecorderPipelineStage::Recording,
                    std::move(step.message), step.write_fault);
      }
      remember_preservation_failure(VideoRecorderPipelineStage::Recording,
                                    step);
      break;
    }
    step = backend->ProcessClipRequests();
    if (!step.succeeded) {
      if (step.failure_disposition ==
          VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
        remember_preservation_failure(VideoRecorderPipelineStage::Recording,
                                      step);
        break;
      }
      return fail(VideoRecorderPipelineStage::Recording,
                  std::move(step.message), step.write_fault);
    }
    if (options.progress_sink &&
        std::chrono::steady_clock::now() >= next_diagnostics_publish) {
      publish_progress();
      next_diagnostics_publish =
          std::chrono::steady_clock::now() + options.diagnostics_interval;
    }
    std::this_thread::sleep_for(options.drain_interval);
  }

  step = backend->ProcessClipRequests();
  if (!step.succeeded) {
    if (step.failure_disposition ==
        VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
      remember_preservation_failure(VideoRecorderPipelineStage::Recording,
                                    step);
    } else {
      return fail(VideoRecorderPipelineStage::Recording,
                  std::move(step.message), step.write_fault);
    }
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::CaptureStop);
  step = backend->StopCapture();
  capture_stopped = true;
  if (!step.succeeded) {
    if (step.failure_disposition !=
        VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
      return fail(VideoRecorderPipelineStage::CaptureStop,
                  std::move(step.message), step.write_fault);
    }
    remember_preservation_failure(VideoRecorderPipelineStage::CaptureStop,
                                  step);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::QueueDrain);
  step = backend->DrainQueuedFrames();
  queues_drained = true;
  if (!step.succeeded) {
    if (step.failure_disposition !=
        VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
      return fail(VideoRecorderPipelineStage::QueueDrain,
                  std::move(step.message), step.write_fault);
    }
    remember_preservation_failure(VideoRecorderPipelineStage::QueueDrain,
                                  step);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::EncoderDrain);
  step = backend->DrainEncoder();
  encoders_drained = true;
  if (!step.succeeded) {
    if (step.failure_disposition !=
        VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
      return fail(VideoRecorderPipelineStage::EncoderDrain,
                  std::move(step.message), step.write_fault);
    }
    remember_preservation_failure(VideoRecorderPipelineStage::EncoderDrain,
                                  step);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::ClipExportDrain);
  step = backend->DrainClipExports();
  clip_exports_drained = true;
  if (!step.succeeded) {
    if (step.failure_disposition ==
        VideoRecorderPipelineFailureDisposition::FinalizeRecording) {
      remember_preservation_failure(
          VideoRecorderPipelineStage::ClipExportDrain, step);
    } else {
      return fail(VideoRecorderPipelineStage::ClipExportDrain,
                  std::move(step.message), step.write_fault);
    }
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::ManifestWrite);
  step = backend->WriteManifest();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::ManifestWrite,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::PacketStoreClose);
  step = backend->ClosePacketStore();
  packet_store_closed = true;
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::PacketStoreClose,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::PacketStoreRecover);
  step = backend->RecoverPacketStore();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::PacketStoreRecover,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::ExportPlan);
  step = backend->BuildExportPlan();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::ExportPlan,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::Mp4Write);
  step = backend->WriteMp4();
  if (!step.succeeded) {
    return fail(VideoRecorderPipelineStage::Mp4Write,
                std::move(step.message), step.write_fault);
  }

  EmitStage(stage_sink, VideoRecorderPipelineStage::Complete);
  if (recording_failure_pending) {
    if (recording_failure_message.empty()) {
      recording_failure_message =
          L"Recording stopped after a recoverable runtime failure.";
    }
    recording_failure_message +=
        L" The recording up to that point was saved to " +
        backend->output_path().wstring() + L".";
    auto failed = Result(FailureStatusForStage(recording_failure_stage),
                         recording_failure_stage,
                         std::move(recording_failure_message),
                         recording_failure_fault);
    failed.recording_saved_after_failure = true;
    return finish(std::move(failed));
  }
  return finish(Result(VideoRecorderPipelineStatus::Success,
                       VideoRecorderPipelineStage::Complete, L""));
}

const wchar_t* VideoRecorderPipelineStageName(
    VideoRecorderPipelineStage stage) noexcept {
  switch (stage) {
    case VideoRecorderPipelineStage::BackendCheck:
      return L"backend check";
    case VideoRecorderPipelineStage::Prepare:
      return L"prepare";
    case VideoRecorderPipelineStage::CaptureStart:
      return L"capture start";
    case VideoRecorderPipelineStage::Recording:
      return L"recording";
    case VideoRecorderPipelineStage::CaptureStop:
      return L"capture stop";
    case VideoRecorderPipelineStage::QueueDrain:
      return L"queue drain";
    case VideoRecorderPipelineStage::EncoderDrain:
      return L"encoder drain";
    case VideoRecorderPipelineStage::ClipExportDrain:
      return L"clip export drain";
    case VideoRecorderPipelineStage::ManifestWrite:
      return L"manifest write";
    case VideoRecorderPipelineStage::PacketStoreClose:
      return L"PacketStore close";
    case VideoRecorderPipelineStage::PacketStoreRecover:
      return L"PacketStore recover";
    case VideoRecorderPipelineStage::ExportPlan:
      return L"export plan";
    case VideoRecorderPipelineStage::Mp4Write:
      return L"MP4 write";
    case VideoRecorderPipelineStage::Complete:
      return L"complete";
  }
  return L"unknown";
}

const wchar_t* VideoRecorderPipelineStatusName(
    VideoRecorderPipelineStatus status) noexcept {
  switch (status) {
    case VideoRecorderPipelineStatus::Success:
      return L"success";
    case VideoRecorderPipelineStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecorderPipelineStatus::Cancelled:
      return L"cancelled";
    case VideoRecorderPipelineStatus::BackendUnavailable:
      return L"backend unavailable";
    case VideoRecorderPipelineStatus::PrepareFailed:
      return L"prepare failed";
    case VideoRecorderPipelineStatus::CaptureStartFailed:
      return L"capture start failed";
    case VideoRecorderPipelineStatus::CaptureFailed:
      return L"capture failed";
    case VideoRecorderPipelineStatus::CaptureStopFailed:
      return L"capture stop failed";
    case VideoRecorderPipelineStatus::QueueDrainFailed:
      return L"queue drain failed";
    case VideoRecorderPipelineStatus::EncoderDrainFailed:
      return L"encoder drain failed";
    case VideoRecorderPipelineStatus::ClipExportDrainFailed:
      return L"clip export drain failed";
    case VideoRecorderPipelineStatus::ManifestWriteFailed:
      return L"manifest write failed";
    case VideoRecorderPipelineStatus::PacketStoreCloseFailed:
      return L"PacketStore close failed";
    case VideoRecorderPipelineStatus::PacketStoreRecoverFailed:
      return L"PacketStore recover failed";
    case VideoRecorderPipelineStatus::ExportPlanFailed:
      return L"export plan failed";
    case VideoRecorderPipelineStatus::Mp4WriteFailed:
      return L"MP4 write failed";
  }
  return L"unknown";
}

}  // namespace olouie::record
