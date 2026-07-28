#include "record/VideoExportPlan.h"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace olouie::record {
namespace {

VideoExportPlanResult Result(VideoExportPlanStatus status,
                             std::wstring message) {
  VideoExportPlanResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
  return left.lexically_normal() == right.lexically_normal();
}

VideoExportTrackMetadata BuildVideoTrackMetadata(
    const VideoTrackSessionManifest& video) {
  VideoExportTrackMetadata metadata;
  metadata.track_id = video.track_id;
  metadata.codec_id = video.codec_id;
  metadata.h264_packet_format = video.h264_packet_format;
  metadata.h264_sps = video.h264_sps;
  metadata.h264_pps = video.h264_pps;
  metadata.h264_avcc_extradata = video.h264_avcc_extradata;
  metadata.width = video.output_width;
  metadata.height = video.output_height;
  metadata.fps_numerator = video.media_fps_numerator;
  metadata.fps_denominator = video.media_fps_denominator;
  return metadata;
}

AudioExportTrackMetadata BuildAudioTrackMetadata(
    const AudioTrackSessionManifest& audio) {
  AudioExportTrackMetadata metadata;
  metadata.track_id = audio.track_id;
  metadata.codec_id = audio.codec_id;
  metadata.source_kind = audio.source_kind;
  metadata.source_index = audio.source_index;
  metadata.name = audio.name;
  metadata.sample_rate = audio.sample_rate;
  metadata.channel_count = audio.channel_count;
  metadata.bitrate_bps = audio.bitrate_bps;
  metadata.aac_frame_samples = audio.aac_frame_samples;
  metadata.aac_payload_type = audio.aac_payload_type;
  metadata.aac_profile_level_indication =
      audio.aac_profile_level_indication;
  metadata.aac_audio_object_type = audio.aac_audio_object_type;
  metadata.aac_audio_specific_config = audio.aac_audio_specific_config;
  metadata.encoder_name = audio.encoder_name;
  return metadata;
}

bool StoreHasManifestTrack(const std::vector<PacketIndexEntry>& index,
                           uint32_t track_id,
                           CodecId codec_id,
                           bool* wrong_codec) {
  bool found = false;
  for (const auto& packet : index) {
    if (packet.metadata.track_id != track_id) {
      continue;
    }

    found = true;
    if (packet.metadata.codec_id != codec_id) {
      if (wrong_codec != nullptr) {
        *wrong_codec = true;
      }
      return false;
    }
  }

  return found;
}

const MuxPacketRef* FindFirstVideoPacket(const MuxPlan& plan,
                                         uint32_t video_track_id) noexcept {
  const auto found = std::find_if(
      plan.packets.begin(), plan.packets.end(),
      [video_track_id](const MuxPacketRef& packet) {
        return packet.packet.metadata.track_id == video_track_id &&
               packet.packet.metadata.codec_id == CodecId::H264;
      });
  return found == plan.packets.end() ? nullptr : &(*found);
}

}  // namespace

bool VideoExportTrackMetadata::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::H264 &&
         h264_packet_format == L"annex_b" && !h264_sps.empty() &&
         !h264_pps.empty() && !h264_avcc_extradata.empty() && width > 0 &&
         height > 0 && fps_numerator > 0 && fps_denominator > 0;
}

bool AudioExportTrackMetadata::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::Aac &&
         !source_kind.empty() && !name.empty() && sample_rate > 0 &&
         channel_count > 0 && bitrate_bps > 0 && aac_frame_samples == 1024 &&
         aac_payload_type == 0 && aac_audio_object_type == 2 &&
         !aac_audio_specific_config.empty() && !encoder_name.empty();
}

bool VideoExportPlan::IsReady() const noexcept {
  if (session_dir.empty() || packet_file_path.empty() || !video.IsReady() ||
      !mux_plan.HasVideoTrack() || mux_plan.packets.empty()) {
    return false;
  }
  std::set<uint32_t> audio_track_ids;
  for (const auto& audio : audio_tracks) {
    if (!audio.IsReady() || !audio_track_ids.insert(audio.track_id).second) {
      return false;
    }
  }
  std::set<uint32_t> omitted_track_ids;
  return std::all_of(
      omitted_audio_track_ids.begin(), omitted_audio_track_ids.end(),
      [&](uint32_t track_id) {
        return track_id != 0 && !audio_track_ids.contains(track_id) &&
               omitted_track_ids.insert(track_id).second;
      });
}

bool VideoExportPlanResult::Succeeded() const noexcept {
  return status == VideoExportPlanStatus::Success;
}

VideoExportPlanResult BuildVideoExportPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan) {
  if (plan == nullptr) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Video export plan needs an output destination.");
  }

  *plan = {};
  if (packet_store == nullptr) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Video export plan needs a PacketStore.");
  }

  PacketStoreExportSnapshot snapshot;
  std::wstring snapshot_error;
  if (!packet_store->SnapshotForExport(&snapshot, &snapshot_error)) {
    return Result(
        VideoExportPlanStatus::PacketStoreSnapshotFailed,
        snapshot_error.empty()
            ? L"Could not snapshot PacketStore for export."
            : std::move(snapshot_error));
  }
  return BuildVideoExportPlan(manifest, snapshot, options, plan);
}

VideoExportPlanResult BuildVideoExportPlan(
    const SessionManifest& manifest,
    const PacketStoreExportSnapshot& snapshot,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan) {
  if (plan == nullptr) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Video export plan needs an output destination.");
  }

  *plan = {};

  if (options.requested_start_ns < 0 ||
      options.requested_end_ns <= options.requested_start_ns) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Video export plan needs a positive session-relative "
                  L"time range.");
  }

  if (!manifest.IsReady()) {
    return Result(VideoExportPlanStatus::InvalidManifest,
                  L"Video export plan needs a ready session manifest.");
  }

  if (!snapshot.IsReady()) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Video export plan needs a ready PacketStore snapshot.");
  }

  if (!SamePath(manifest.session_dir, snapshot.session_dir) ||
      !SamePath(manifest.packet_file_path, snapshot.packet_file_path)) {
    return Result(VideoExportPlanStatus::MetadataMismatch,
                  L"Session manifest paths do not match the PacketStore.");
  }

  const auto& index = snapshot.index;
  bool wrong_store_codec = false;
  if (!StoreHasManifestTrack(index, manifest.video.track_id,
                             manifest.video.codec_id,
                             &wrong_store_codec)) {
    return Result(wrong_store_codec ? VideoExportPlanStatus::MetadataMismatch
                                    : VideoExportPlanStatus::NoPackets,
                  wrong_store_codec
                      ? L"PacketStore video track codec does not match the "
                        L"session manifest."
                      : L"PacketStore does not contain the manifest video "
                        L"track.");
  }

  for (const auto& audio : manifest.audio_tracks) {
    wrong_store_codec = false;
    if (!StoreHasManifestTrack(index, audio.track_id, audio.codec_id,
                               &wrong_store_codec) &&
        wrong_store_codec) {
      return Result(VideoExportPlanStatus::MetadataMismatch,
                    L"PacketStore audio track codec does not match the "
                    L"session manifest.");
    }
  }

  const auto range = QueryPacketRange(
      snapshot.index,
      options.requested_start_ns, options.requested_end_ns,
      options.include_previous_keyframe);

  std::map<uint32_t, CodecId> manifest_tracks{
      {manifest.video.track_id, manifest.video.codec_id}};
  std::vector<TrackDefinition> track_definitions{
      {manifest.video.track_id, manifest.video.codec_id}};
  for (const auto& audio : manifest.audio_tracks) {
    if (!manifest_tracks.emplace(audio.track_id, audio.codec_id).second) {
      return Result(VideoExportPlanStatus::InvalidManifest,
                    L"Session manifest contains duplicate track ids.");
    }
    track_definitions.push_back({audio.track_id, audio.codec_id});
  }

  PacketRange export_range;
  export_range.requested_start_ns = range.requested_start_ns;
  export_range.requested_end_ns = range.requested_end_ns;
  export_range.actual_start_ns = range.actual_start_ns;
  export_range.actual_end_ns = range.actual_end_ns;
  std::set<uint32_t> ranged_track_ids;
  for (const auto& packet : range.packets) {
    const auto track = manifest_tracks.find(packet.metadata.track_id);
    if (track == manifest_tracks.end()) {
      if (manifest.version != kLegacyVideoOnlySessionManifestVersion) {
        return Result(VideoExportPlanStatus::MetadataMismatch,
                      L"Requested range contains a packet track omitted from "
                      L"the current session manifest.");
      }
      continue;
    }
    if (track->second != packet.metadata.codec_id) {
      return Result(VideoExportPlanStatus::MetadataMismatch,
                    L"Requested range packet codec does not match its "
                    L"session manifest track.");
    }
    export_range.packets.push_back(packet);
    ranged_track_ids.insert(packet.metadata.track_id);
  }

  if (!ranged_track_ids.contains(manifest.video.track_id)) {
    return Result(VideoExportPlanStatus::NoPackets,
                  L"Requested video range does not contain video packets.");
  }
  if (options.require_all_audio_tracks) {
    for (const auto& audio : manifest.audio_tracks) {
      if (!ranged_track_ids.contains(audio.track_id)) {
        return Result(
            VideoExportPlanStatus::MissingAudioPackets,
            L"Requested range is missing required audio track " +
                std::to_wstring(audio.track_id) + L" (" + audio.name +
                L"). The recording was not exported as complete.");
      }
    }
  }
  MuxPlanOptions mux_options;
  mux_options.require_video_track = true;
  mux_options.normalize_timestamps = options.normalize_timestamps;

  MuxPlan mux_plan;
  std::wstring mux_error;
  if (!BuildMuxPlan(export_range,
                    std::span<const TrackDefinition>(track_definitions),
                    mux_options, &mux_plan, &mux_error)) {
    return Result(VideoExportPlanStatus::MuxPlanFailed,
                  mux_error.empty() ? L"Video export mux plan failed."
                                    : std::move(mux_error));
  }

  const auto* first_video_packet =
      FindFirstVideoPacket(mux_plan, manifest.video.track_id);
  if (options.require_keyframe_start &&
      (first_video_packet == nullptr ||
       !first_video_packet->packet.IsKeyframe())) {
    return Result(VideoExportPlanStatus::NoKeyframe,
                  L"Video export plan must start on a keyframe.");
  }

  VideoExportPlan built;
  built.session_dir = manifest.session_dir;
  built.packet_file_path = manifest.packet_file_path;
  built.video = BuildVideoTrackMetadata(manifest.video);
  built.audio_tracks.reserve(manifest.audio_tracks.size());
  built.omitted_audio_track_ids.reserve(manifest.audio_tracks.size());
  // The manifest records configured sources. MP4 streams follow packet truth so
  // an entirely silent selected range does not require fabricated AAC packets.
  for (const auto& audio : manifest.audio_tracks) {
    if (ranged_track_ids.contains(audio.track_id)) {
      built.audio_tracks.push_back(BuildAudioTrackMetadata(audio));
    } else {
      built.omitted_audio_track_ids.push_back(audio.track_id);
    }
  }
  built.mux_plan = std::move(mux_plan);

  if (!built.IsReady()) {
    return Result(VideoExportPlanStatus::InvalidManifest,
                  L"Video export plan metadata is incomplete.");
  }

  *plan = std::move(built);
  return Result(VideoExportPlanStatus::Success, L"");
}

VideoExportPlanResult BuildRecoveredVideoExportPlan(
    const std::filesystem::path& session_dir,
    const VideoExportPlanOptions& options,
    VideoExportPlan* plan) {
  if (plan == nullptr || session_dir.empty()) {
    return Result(VideoExportPlanStatus::InvalidRequest,
                  L"Recovered video export plan needs a session directory and "
                  L"output destination.");
  }

  *plan = {};

  SessionManifest manifest;
  const auto manifest_read = ReadSessionManifest(session_dir, &manifest);
  if (!manifest_read.Succeeded()) {
    std::wstring message = L"Session manifest read failed: ";
    message += SessionManifestStatusName(manifest_read.status);
    if (!manifest_read.message.empty()) {
      message += L" - ";
      message += manifest_read.message;
    }
    return Result(VideoExportPlanStatus::ManifestReadFailed,
                  std::move(message));
  }

  std::wstring recover_error;
  auto recovered = PacketStore::Recover(session_dir, &recover_error);
  if (!recover_error.empty() || recovered.session_dir().empty()) {
    return Result(VideoExportPlanStatus::PacketStoreRecoverFailed,
                  recover_error.empty()
                      ? L"PacketStore recovery did not produce a session."
                      : std::move(recover_error));
  }

  return BuildVideoExportPlan(manifest, &recovered, options, plan);
}

const wchar_t* VideoExportPlanStatusName(
    VideoExportPlanStatus status) noexcept {
  switch (status) {
    case VideoExportPlanStatus::Success:
      return L"success";
    case VideoExportPlanStatus::InvalidRequest:
      return L"invalid request";
    case VideoExportPlanStatus::InvalidManifest:
      return L"invalid manifest";
    case VideoExportPlanStatus::ManifestReadFailed:
      return L"manifest read failed";
    case VideoExportPlanStatus::PacketStoreRecoverFailed:
      return L"packet store recover failed";
    case VideoExportPlanStatus::PacketStoreSnapshotFailed:
      return L"packet store snapshot failed";
    case VideoExportPlanStatus::MetadataMismatch:
      return L"metadata mismatch";
    case VideoExportPlanStatus::NoPackets:
      return L"no packets";
    case VideoExportPlanStatus::MissingAudioPackets:
      return L"missing audio packets";
    case VideoExportPlanStatus::NoKeyframe:
      return L"no keyframe";
    case VideoExportPlanStatus::MuxPlanFailed:
      return L"mux plan failed";
  }

  return L"unknown";
}

}  // namespace olouie::record
