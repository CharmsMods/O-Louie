#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "record/RecordingRecovery.h"
#include "record/VideoRecorderSession.h"
#include "settings/Settings.h"

namespace olouie::diagnostics {

struct DiagnosticsAudioTrack {
  uint32_t track_id = 0;
  std::wstring name;
  bool packet_bearing = false;
};

struct DiagnosticsSnapshot {
  uint64_t generation = 0;
  record::VideoRecorderState recorder_state =
      record::VideoRecorderState::Idle;
  record::RecordingRecoveryState recovery_state =
      record::RecordingRecoveryState::Idle;
  std::wstring recorder_message;
  std::wstring recovery_message;
  std::wstring monitor_identity;
  std::wstring encoder_identity;
  double requested_fps = 0.0;
  double negotiated_fps = 0.0;
  double observed_fps = 0.0;
  uint64_t requested_bitrate_bps = 0;
  uint64_t negotiated_bitrate_bps = 0;
  uint64_t observed_bitrate_bps = 0;
  uint64_t recording_elapsed_ns = 0;
  record::VideoRecorderPipelineStats recorder_stats;
  std::vector<DiagnosticsAudioTrack> audio_tracks;
  size_t recoverable_session_count = 0;
  size_t discardable_session_count = 0;
  std::filesystem::path session_directory;
  std::filesystem::path recording_output_path;
  std::filesystem::path configured_output_directory;
  std::filesystem::path latest_export_path;
  std::wstring latest_export_state;
  std::wstring first_actionable_failure;
};

DiagnosticsSnapshot BuildDiagnosticsSnapshot(
    const settings::AppSettings& settings,
    const record::VideoRecorderSnapshot& recorder,
    const record::RecordingRecoverySnapshot& recovery);

std::wstring FormatDiagnosticsReport(const DiagnosticsSnapshot& snapshot);

}  // namespace olouie::diagnostics
