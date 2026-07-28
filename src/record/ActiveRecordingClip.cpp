#include "record/ActiveRecordingClip.h"

#include <algorithm>
#include <utility>

#include "record/VideoExportPlan.h"

namespace olouie::record {
namespace {

ActiveRecordingClipResult Result(ActiveRecordingClipStatus status,
                                 std::wstring message) {
  return {status, std::move(message)};
}

}  // namespace

bool ActiveRecordingClipPlan::IsReady() const noexcept {
  return requested_duration_ns > 0 && available_end_ns > 0 &&
         clamped_start_ns >= 0 && actual_end_ns > actual_start_ns &&
         !mux_request.packet_file_path.empty() &&
         !mux_request.final_output_path.empty() &&
         !mux_request.plan.packets.empty();
}

bool ActiveRecordingClipResult::Succeeded() const noexcept {
  return status == ActiveRecordingClipStatus::Success;
}

ActiveRecordingClipResult BuildActiveRecordingClipPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    const ActiveRecordingClipOptions& options,
    ActiveRecordingClipPlan* plan) {
  if (plan == nullptr || packet_store == nullptr ||
      options.duration_ns <= 0 || options.temp_output_path.empty() ||
      options.final_output_path.empty()) {
    return Result(ActiveRecordingClipStatus::InvalidRequest,
                  L"Active clip planning needs a positive duration, output "
                  L"paths, PacketStore, and destination.");
  }
  *plan = {};

  PacketStoreExportSnapshot snapshot;
  std::wstring snapshot_error;
  if (!packet_store->SnapshotForExport(&snapshot, &snapshot_error)) {
    return Result(
        ActiveRecordingClipStatus::PacketStoreSnapshotFailed,
        snapshot_error.empty()
            ? L"Could not snapshot the active PacketStore."
            : std::move(snapshot_error));
  }

  int64_t available_end_ns = 0;
  for (const auto& packet : snapshot.index) {
    if (packet.metadata.track_id == manifest.video.track_id &&
        packet.metadata.codec_id == CodecId::H264) {
      available_end_ns = std::max(available_end_ns, packet.EndPtsNs());
    }
  }
  if (available_end_ns <= 0) {
    return Result(ActiveRecordingClipStatus::NoVideoPackets,
                  L"The active recording does not yet contain video packets.");
  }

  const int64_t clamped_start_ns =
      options.duration_ns >= available_end_ns
          ? 0
          : available_end_ns - options.duration_ns;

  VideoExportPlanOptions export_options;
  export_options.requested_start_ns = clamped_start_ns;
  export_options.requested_end_ns = available_end_ns;
  export_options.include_previous_keyframe = true;
  export_options.require_keyframe_start = true;
  export_options.normalize_timestamps = true;
  export_options.require_all_audio_tracks = true;

  VideoExportPlan export_plan;
  const auto export_result = BuildVideoExportPlan(
      manifest, snapshot, export_options, &export_plan);
  if (!export_result.Succeeded()) {
    return Result(
        ActiveRecordingClipStatus::ExportPlanFailed,
        export_result.message.empty()
            ? L"Could not build the active clip export plan."
            : export_result.message);
  }

  Mp4MuxRequest mux_request;
  const auto mux_result = BuildVideoMp4MuxRequest(
      export_plan, options.temp_output_path, options.final_output_path,
      options.allow_overwrite, &mux_request);
  if (!mux_result.Succeeded()) {
    return Result(
        ActiveRecordingClipStatus::MuxRequestFailed,
        mux_result.message.empty()
            ? L"Could not build the active clip MP4 request."
            : mux_result.message);
  }

  ActiveRecordingClipPlan built;
  built.requested_duration_ns = options.duration_ns;
  built.available_end_ns = available_end_ns;
  built.clamped_start_ns = clamped_start_ns;
  built.actual_start_ns = export_plan.mux_plan.source_start_ns;
  built.actual_end_ns = export_plan.mux_plan.source_end_ns;
  built.omitted_audio_track_ids =
      std::move(export_plan.omitted_audio_track_ids);
  built.mux_request = std::move(mux_request);
  if (!built.IsReady()) {
    return Result(ActiveRecordingClipStatus::MuxRequestFailed,
                  L"Active clip planning produced an incomplete MP4 request.");
  }

  *plan = std::move(built);
  return Result(ActiveRecordingClipStatus::Success, L"");
}

const wchar_t* ActiveRecordingClipStatusName(
    ActiveRecordingClipStatus status) noexcept {
  switch (status) {
    case ActiveRecordingClipStatus::Success:
      return L"success";
    case ActiveRecordingClipStatus::InvalidRequest:
      return L"invalid request";
    case ActiveRecordingClipStatus::PacketStoreSnapshotFailed:
      return L"packet store snapshot failed";
    case ActiveRecordingClipStatus::NoVideoPackets:
      return L"no video packets";
    case ActiveRecordingClipStatus::ExportPlanFailed:
      return L"export plan failed";
    case ActiveRecordingClipStatus::MuxRequestFailed:
      return L"mux request failed";
  }
  return L"unknown";
}

}  // namespace olouie::record
