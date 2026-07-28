#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "record/Mp4Muxer.h"
#include "record/PacketStore.h"
#include "record/SessionManifest.h"

namespace olouie::record {

enum class ActiveRecordingClipStatus {
  Success,
  InvalidRequest,
  PacketStoreSnapshotFailed,
  NoVideoPackets,
  ExportPlanFailed,
  MuxRequestFailed,
};

struct ActiveRecordingClipOptions {
  int64_t duration_ns = 0;
  std::filesystem::path temp_output_path;
  std::filesystem::path final_output_path;
  bool allow_overwrite = false;
};

struct ActiveRecordingClipPlan {
  int64_t requested_duration_ns = 0;
  int64_t available_end_ns = 0;
  int64_t clamped_start_ns = 0;
  int64_t actual_start_ns = 0;
  int64_t actual_end_ns = 0;
  std::vector<uint32_t> omitted_audio_track_ids;
  Mp4MuxRequest mux_request;

  bool IsReady() const noexcept;
};

struct ActiveRecordingClipResult {
  ActiveRecordingClipStatus status =
      ActiveRecordingClipStatus::InvalidRequest;
  std::wstring message;

  bool Succeeded() const noexcept;
};

ActiveRecordingClipResult BuildActiveRecordingClipPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    const ActiveRecordingClipOptions& options,
    ActiveRecordingClipPlan* plan);

const wchar_t* ActiveRecordingClipStatusName(
    ActiveRecordingClipStatus status) noexcept;

}  // namespace olouie::record
