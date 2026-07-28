#include "diagnostics/DiagnosticsSnapshot.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace olouie::diagnostics {
namespace {

double Rate(uint32_t numerator, uint32_t denominator) {
  return denominator == 0
             ? 0.0
             : static_cast<double>(numerator) /
                   static_cast<double>(denominator);
}

std::wstring PathOrUnavailable(const std::filesystem::path& path) {
  return path.empty() ? L"Unavailable" : path.wstring();
}

}  // namespace

DiagnosticsSnapshot BuildDiagnosticsSnapshot(
    const settings::AppSettings& settings,
    const record::VideoRecorderSnapshot& recorder,
    const record::RecordingRecoverySnapshot& recovery) {
  DiagnosticsSnapshot snapshot;
  snapshot.generation = recorder.diagnostics_generation +
                        recorder.clip_event_generation + recovery.generation +
                        recovery.action_generation;
  snapshot.recorder_state = recorder.state;
  snapshot.recovery_state = recovery.state;
  snapshot.recorder_message = recorder.message;
  snapshot.recovery_message = recovery.message;
  snapshot.recorder_stats = recorder.stats;
  snapshot.session_directory = recorder.session_directory;
  snapshot.recording_output_path = recorder.output_path;
  snapshot.configured_output_directory = settings.output_directory;
  snapshot.recoverable_session_count = recovery.scan.ExportableCount();
  snapshot.discardable_session_count = recovery.scan.DiscardableCount();

  const auto& runtime = recorder.stats.runtime;
  snapshot.monitor_identity = runtime.monitor_device_name;
  if (snapshot.monitor_identity.empty()) {
    snapshot.monitor_identity = settings.video.monitor_device_name.empty()
                                    ? L"Primary monitor (configured)"
                                    : settings.video.monitor_device_name;
  }
  snapshot.encoder_identity = runtime.encoder_name.empty()
                                  ? L"Media Foundation hardware H.264 (configured)"
                                  : runtime.encoder_name;

  const uint32_t requested_fps_numerator =
      runtime.requested_fps_numerator == 0
          ? static_cast<uint32_t>(std::max(0, settings.video.fps))
          : runtime.requested_fps_numerator;
  const uint32_t requested_fps_denominator =
      runtime.requested_fps_denominator == 0
          ? 1
          : runtime.requested_fps_denominator;
  snapshot.requested_fps =
      Rate(requested_fps_numerator, requested_fps_denominator);
  snapshot.negotiated_fps = Rate(runtime.negotiated_fps_numerator,
                                 runtime.negotiated_fps_denominator);
  snapshot.requested_bitrate_bps =
      runtime.requested_bitrate_bps == 0
          ? static_cast<uint64_t>(std::max(0, settings.video.bitrate_mbps)) *
                1000000ULL
          : runtime.requested_bitrate_bps;
  snapshot.negotiated_bitrate_bps = runtime.negotiated_bitrate_bps;
  snapshot.recording_elapsed_ns = runtime.recording_elapsed_ns;
  if (runtime.recording_elapsed_ns != 0) {
    const double seconds =
        static_cast<double>(runtime.recording_elapsed_ns) / 1000000000.0;
    snapshot.observed_fps =
        static_cast<double>(recorder.stats.encoded_frame_count) / seconds;
    snapshot.observed_bitrate_bps = static_cast<uint64_t>(std::llround(
        static_cast<double>(runtime.encoded_video_payload_bytes) * 8.0 /
        seconds));
  }

  for (const auto& track : runtime.audio_tracks) {
    snapshot.audio_tracks.push_back(
        {track.track_id, track.name, track.packet_bearing});
  }
  if (snapshot.audio_tracks.empty()) {
    if (settings.audio.system_mix) {
      snapshot.audio_tracks.push_back({0, L"System audio (configured)", false});
    }
    if (settings.audio.mic) {
      snapshot.audio_tracks.push_back({0, L"Microphone (configured)", false});
    }
  }

  if (recorder.clip.state != record::VideoRecorderClipState::None) {
    snapshot.latest_export_state =
        std::wstring(record::VideoRecorderExportKindName(recorder.clip.kind)) +
        L" " + record::VideoRecorderClipStateName(recorder.clip.state);
    snapshot.latest_export_path = recorder.clip.output_path;
  } else if (recovery.action_kind !=
             record::RecordingRecoveryActionKind::None) {
    snapshot.latest_export_state =
        recovery.action.Succeeded() ? L"recovery saved" : L"recovery failed";
    snapshot.latest_export_path = recovery.action.output_path;
  }

  if (recorder.state == record::VideoRecorderState::Failed) {
    snapshot.first_actionable_failure = recorder.message;
  } else if (recorder.clip.state == record::VideoRecorderClipState::Failed) {
    snapshot.first_actionable_failure = recorder.clip.message;
  } else if (recovery.state == record::RecordingRecoveryState::Failed) {
    snapshot.first_actionable_failure = recovery.message;
  } else if (recovery.action_kind !=
                 record::RecordingRecoveryActionKind::None &&
             !recovery.action.Succeeded()) {
    snapshot.first_actionable_failure = recovery.action.message;
  }
  return snapshot;
}

std::wstring FormatDiagnosticsReport(const DiagnosticsSnapshot& snapshot) {
  std::wostringstream report;
  report.setf(std::ios::fixed);
  report.precision(2);
  report << L"Recorder: " << record::VideoRecorderStateName(snapshot.recorder_state)
         << L'\n'
         << L"Recovery: "
         << record::RecordingRecoveryStateName(snapshot.recovery_state) << L'\n'
         << L"Monitor: " << snapshot.monitor_identity << L'\n'
         << L"Encoder: " << snapshot.encoder_identity << L'\n'
         << L"Recording performance mode: "
         << performance::CapturePerformanceModeName(
                snapshot.recorder_stats.runtime.performance_mode)
         << L'\n'
         << L"Process priority class / real-time: "
         << snapshot.recorder_stats.runtime.process_priority_class << L" / "
         << (snapshot.recorder_stats.runtime.realtime_process_priority
                 ? L"yes"
                 : L"no")
         << L'\n'
         << L"Recorder MMCSS registered / priority applied: "
         << (snapshot.recorder_stats.runtime.recorder_scheduling.registered
                 ? L"yes"
                 : L"no")
         << L" / "
         << (snapshot.recorder_stats.runtime.recorder_scheduling
                     .priority_applied
                 ? L"yes"
                 : L"no")
         << L'\n'
         << L"Capture MMCSS registered / priority applied: "
         << (snapshot.recorder_stats.runtime.capture_scheduling.registered
                 ? L"yes"
                 : L"no")
         << L" / "
         << (snapshot.recorder_stats.runtime.capture_scheduling
                     .priority_applied
                 ? L"yes"
                 : L"no")
         << L'\n'
         << L"Video encode MMCSS registered / priority applied: "
         << (snapshot.recorder_stats.runtime.video_encode_scheduling.registered
                 ? L"yes"
                 : L"no")
         << L" / "
         << (snapshot.recorder_stats.runtime.video_encode_scheduling
                     .priority_applied
                 ? L"yes"
                 : L"no")
         << L'\n'
         << L"Audio capture MMCSS applied / sources: "
         << std::count_if(
                snapshot.recorder_stats.runtime.audio_capture_scheduling
                    .begin(),
                snapshot.recorder_stats.runtime.audio_capture_scheduling.end(),
                [](const auto& scheduling) {
                  return scheduling.Succeeded();
                })
         << L" / "
         << snapshot.recorder_stats.runtime.audio_capture_scheduling.size()
         << L'\n'
         << L"FPS requested / negotiated / observed: "
         << snapshot.requested_fps << L" / " << snapshot.negotiated_fps
         << L" / " << snapshot.observed_fps << L'\n'
         << L"Bitrate bps requested / negotiated / observed: "
         << snapshot.requested_bitrate_bps << L" / "
         << snapshot.negotiated_bitrate_bps << L" / "
         << snapshot.observed_bitrate_bps << L'\n'
         << L"Frames captured / accepted / rate-limited / dropped / encoded: "
         << snapshot.recorder_stats.captured_frame_count << L" / "
         << snapshot.recorder_stats.accepted_frame_count << L" / "
         << snapshot.recorder_stats.rate_limited_frame_count << L" / "
         << snapshot.recorder_stats.dropped_frame_count << L" / "
         << snapshot.recorder_stats.encoded_frame_count << L'\n'
         << L"Video queue current / peak / capacity: "
         << snapshot.recorder_stats.runtime.queued_video_frame_count << L" / "
         << snapshot.recorder_stats.runtime.peak_queued_video_frame_count
         << L" / " << snapshot.recorder_stats.runtime.video_queue_capacity
         << L'\n'
         << L"Video queue age current / maximum ns: "
         << snapshot.recorder_stats.runtime.video_queue_oldest_frame_age_ns
         << L" / "
         << snapshot.recorder_stats.runtime.video_queue_maximum_frame_age_ns
         << L'\n'
         << L"Video queue overflows / stale-backlog recoveries: "
         << snapshot.recorder_stats.runtime.video_queue_overflow_event_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_queue_backlog_recovery_count
         << L'\n'
         << L"Video queue drops newest / oldest / backlog: "
         << snapshot.recorder_stats.runtime.video_queue_dropped_newest_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_queue_dropped_oldest_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_queue_dropped_backlog_count
         << L'\n'
         << L"Video queue last overflow: "
         << capture::VideoFrameQueueOverflowReasonName(
                snapshot.recorder_stats.runtime
                    .video_queue_last_overflow_reason)
         << L'\n'
         << L"Capture texture pool allocated / in use / peak / capacity: "
         << snapshot.recorder_stats.runtime.video_texture_pool
                .allocated_texture_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_texture_pool
                .in_use_texture_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_texture_pool
                .peak_in_use_texture_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_texture_pool.capacity
         << L'\n'
         << L"Capture texture pool created / reused / exhausted frames: "
         << snapshot.recorder_stats.runtime.video_texture_pool
                .created_texture_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_texture_pool
                .reused_texture_count
         << L" / "
         << snapshot.recorder_stats.runtime
                .video_texture_pool_exhausted_frame_count
         << L'\n'
         << L"Converter input views created / reused / evicted: "
         << snapshot.recorder_stats.runtime.video_converter
                .input_view_create_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_converter
                .input_view_reuse_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_converter
                .input_view_eviction_count
         << L'\n'
         << L"Converter output views created / reused: "
         << snapshot.recorder_stats.runtime.video_converter
                .output_view_create_count
         << L" / "
         << snapshot.recorder_stats.runtime.video_converter
                .output_view_reuse_count
         << L'\n'
         << L"Capture copy submission latency last / maximum / average ns: "
         << snapshot.recorder_stats.runtime.video_capture_copy_last_latency_ns
         << L" / "
         << snapshot.recorder_stats.runtime
                .video_capture_copy_maximum_latency_ns
         << L" / "
         << (snapshot.recorder_stats.runtime
                     .video_capture_copy_submission_count == 0
                 ? 0
                 : snapshot.recorder_stats.runtime
                           .video_capture_copy_total_latency_ns /
                       snapshot.recorder_stats.runtime
                           .video_capture_copy_submission_count)
         << L'\n'
         << L"Video queue wait last / maximum / average ns: "
         << snapshot.recorder_stats.runtime.video_queue_wait_last_ns
         << L" / "
         << snapshot.recorder_stats.runtime.video_queue_wait_maximum_ns
         << L" / "
         << (snapshot.recorder_stats.runtime.video_queue_wait_sample_count == 0
                 ? 0
                 : snapshot.recorder_stats.runtime.video_queue_wait_total_ns /
                       snapshot.recorder_stats.runtime
                           .video_queue_wait_sample_count)
         << L'\n'
         << L"Conversion submission latency last / maximum / average ns: "
         << snapshot.recorder_stats.runtime.video_converter
                .last_conversion_submission_latency_ns
         << L" / "
         << snapshot.recorder_stats.runtime.video_converter
                .maximum_conversion_submission_latency_ns
         << L" / "
         << (snapshot.recorder_stats.runtime.video_converter
                     .conversion_submission_count == 0
                 ? 0
                 : snapshot.recorder_stats.runtime.video_converter
                           .total_conversion_submission_latency_ns /
                       snapshot.recorder_stats.runtime.video_converter
                           .conversion_submission_count)
         << L'\n'
         << L"Encoder wait last / maximum / average ns: "
         << snapshot.recorder_stats.runtime.video_encoder_wait_last_ns
         << L" / "
         << snapshot.recorder_stats.runtime.video_encoder_wait_maximum_ns
         << L" / "
         << (snapshot.recorder_stats.runtime.video_encoder_wait_count == 0
                 ? 0
                 : snapshot.recorder_stats.runtime
                           .video_encoder_wait_total_ns /
                       snapshot.recorder_stats.runtime.video_encoder_wait_count)
         << L'\n'
         << L"Packet writer queue packets current / peak, bytes current / "
            L"peak: "
         << snapshot.recorder_stats.runtime.packet_writer.queued_packet_count
         << L" / "
         << snapshot.recorder_stats.runtime.packet_writer
                .peak_queued_packet_count
         << L", "
         << snapshot.recorder_stats.runtime.packet_writer.queued_payload_bytes
         << L" / "
         << snapshot.recorder_stats.runtime.packet_writer
                .peak_queued_payload_bytes
         << L'\n'
         << L"Packet write latency last / maximum / average ns: "
         << snapshot.recorder_stats.runtime.packet_writer.last_write_latency_ns
         << L" / "
         << snapshot.recorder_stats.runtime.packet_writer
                .maximum_write_latency_ns
         << L" / "
         << (snapshot.recorder_stats.runtime.packet_writer
                     .persisted_packet_count == 0
                 ? 0
                 : snapshot.recorder_stats.runtime.packet_writer
                           .total_write_latency_ns /
                       snapshot.recorder_stats.runtime.packet_writer
                           .persisted_packet_count)
         << L'\n'
         << L"Audio blocks queued / peak / capacity per track: "
         << snapshot.recorder_stats.runtime.queued_audio_block_count << L" / "
         << snapshot.recorder_stats.runtime.peak_queued_audio_block_count
         << L" / "
         << snapshot.recorder_stats.runtime.audio_queue_capacity_per_track
         << L'\n'
         << L"Audio discontinuities / timestamp errors / retimed packets: "
         << snapshot.recorder_stats.audio_data_discontinuity_count << L" / "
         << snapshot.recorder_stats.audio_timestamp_error_count << L" / "
         << snapshot.recorder_stats.audio_retimed_packet_count << L'\n'
         << L"Audio invalidations / default changes / restart attempts / "
            L"successes: "
         << snapshot.recorder_stats.audio_endpoint_invalidation_count
         << L" / "
         << snapshot.recorder_stats.audio_default_device_change_count
         << L" / " << snapshot.recorder_stats.audio_restart_attempt_count
         << L" / " << snapshot.recorder_stats.audio_restart_success_count
         << L'\n'
         << L"Audio capture format changes: "
         << snapshot.recorder_stats.audio_capture_format_change_count << L'\n'
         << L"Continuity silence packets / frames: "
         << snapshot.recorder_stats.synthetic_audio_silence_packet_count
         << L" / "
         << snapshot.recorder_stats.synthetic_audio_silence_frame_count
         << L'\n'
         << L"Exports pending commands / outstanding / saved / failed: "
         << snapshot.recorder_stats.runtime.pending_export_command_count
         << L" / "
         << snapshot.recorder_stats.runtime.outstanding_export_count << L" / "
         << snapshot.recorder_stats.runtime.saved_export_count << L" / "
         << snapshot.recorder_stats.runtime.failed_export_count << L'\n'
         << L"Session: " << PathOrUnavailable(snapshot.session_directory)
         << L'\n'
         << L"Recording output: "
         << PathOrUnavailable(snapshot.recording_output_path) << L'\n'
         << L"Configured output: "
         << PathOrUnavailable(snapshot.configured_output_directory) << L'\n';
  if (!snapshot.latest_export_state.empty()) {
    report << L"Latest export: " << snapshot.latest_export_state << L" - "
           << PathOrUnavailable(snapshot.latest_export_path) << L'\n';
  }
  report << L"Encoder codec controls:";
  if (snapshot.recorder_stats.runtime.encoder_codec_settings.empty()) {
    report << L" unavailable\n";
  } else {
    report << L'\n';
    for (const auto& setting :
         snapshot.recorder_stats.runtime.encoder_codec_settings) {
      report << L"  " << setting.name << L": requested="
             << setting.requested_value << L", supported="
             << (setting.supported ? L"yes" : L"no")
             << L", modifiable="
             << (setting.modifiable ? L"yes" : L"no")
             << L", applied=" << (setting.applied ? L"yes" : L"no");
      if (setting.read_back) {
        report << L", accepted=" << setting.accepted_value;
      }
      if (!setting.message.empty()) {
        report << L" (" << setting.message << L")";
      }
      report << L'\n';
    }
  }
  report << L"Audio tracks:";
  if (snapshot.audio_tracks.empty()) {
    report << L" none\n";
  } else {
    report << L'\n';
    for (const auto& track : snapshot.audio_tracks) {
      report << L"  " << track.name << L" (track " << track.track_id << L", "
             << (track.packet_bearing ? L"packet-bearing" : L"no packets")
             << L")\n";
    }
  }
  if (!snapshot.first_actionable_failure.empty()) {
    report << L"Failure: " << snapshot.first_actionable_failure << L'\n';
  }
  return report.str();
}

}  // namespace olouie::diagnostics
