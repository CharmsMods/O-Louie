#include "record/MuxPlan.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace olouie::record {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsKnownCodec(CodecId codec_id) {
  return codec_id == CodecId::H264 || codec_id == CodecId::Aac;
}

int64_t NormalizeTimestamp(int64_t timestamp_ns, int64_t source_start_ns,
                           bool normalize) {
  if (!normalize) {
    return timestamp_ns;
  }

  return std::max<int64_t>(0, timestamp_ns - source_start_ns);
}

}  // namespace

bool MuxPlan::HasVideoTrack() const noexcept {
  return std::any_of(tracks.begin(), tracks.end(), [](const MuxTrack& track) {
    return track.codec_id == CodecId::H264;
  });
}

bool BuildMuxPlan(const PacketRange& range,
                  std::span<const TrackDefinition> track_definitions,
                  const MuxPlanOptions& options, MuxPlan* plan,
                  std::wstring* error) {
  if (plan == nullptr) {
    SetError(error, L"MuxPlan needs an output destination.");
    return false;
  }

  if (range.actual_end_ns <= range.actual_start_ns) {
    SetError(error, L"MuxPlan range must have a positive duration.");
    return false;
  }

  if (range.packets.empty()) {
    SetError(error, L"MuxPlan needs at least one packet.");
    return false;
  }

  std::map<uint32_t, CodecId> track_codecs;
  for (const auto& track : track_definitions) {
    if (track.track_id == 0 || !IsKnownCodec(track.codec_id)) {
      SetError(error, L"MuxPlan track definitions must be valid.");
      return false;
    }

    const auto [_, inserted] =
        track_codecs.try_emplace(track.track_id, track.codec_id);
    if (!inserted) {
      SetError(error, L"MuxPlan track definitions contain duplicate ids.");
      return false;
    }
  }

  std::set<uint32_t> used_track_ids;
  std::vector<MuxPacketRef> mux_packets;
  mux_packets.reserve(range.packets.size());

  for (const auto& packet : range.packets) {
    const auto found = track_codecs.find(packet.metadata.track_id);
    if (found == track_codecs.end()) {
      SetError(error, L"MuxPlan packet references an unknown track id.");
      return false;
    }

    if (found->second != packet.metadata.codec_id) {
      SetError(error, L"MuxPlan packet codec does not match its track.");
      return false;
    }

    if (packet.metadata.duration_ns < 0) {
      SetError(error, L"MuxPlan packet duration must not be negative.");
      return false;
    }

    MuxPacketRef ref;
    ref.packet = packet;
    ref.output_pts_ns = NormalizeTimestamp(
        packet.metadata.pts_ns, range.actual_start_ns,
        options.normalize_timestamps);
    ref.output_dts_ns = NormalizeTimestamp(
        packet.metadata.dts_ns, range.actual_start_ns,
        options.normalize_timestamps);
    ref.duration_ns = packet.metadata.duration_ns;
    mux_packets.push_back(ref);
    used_track_ids.insert(packet.metadata.track_id);
  }

  std::vector<MuxTrack> mux_tracks;
  mux_tracks.reserve(used_track_ids.size());
  for (const auto& track : track_definitions) {
    if (used_track_ids.contains(track.track_id)) {
      mux_tracks.push_back({track.track_id, track.codec_id});
    }
  }

  if (options.require_video_track &&
      std::none_of(mux_tracks.begin(), mux_tracks.end(),
                   [](const MuxTrack& track) {
                     return track.codec_id == CodecId::H264;
                   })) {
    SetError(error, L"MuxPlan requires a video track.");
    return false;
  }

  std::sort(mux_packets.begin(), mux_packets.end(),
            [](const MuxPacketRef& left, const MuxPacketRef& right) {
              if (left.output_dts_ns != right.output_dts_ns) {
                return left.output_dts_ns < right.output_dts_ns;
              }
              if (left.output_pts_ns != right.output_pts_ns) {
                return left.output_pts_ns < right.output_pts_ns;
              }
              if (left.packet.metadata.track_id !=
                  right.packet.metadata.track_id) {
                return left.packet.metadata.track_id <
                       right.packet.metadata.track_id;
              }
              return left.packet.file_offset < right.packet.file_offset;
            });

  MuxPlan built;
  built.source_start_ns = range.actual_start_ns;
  built.source_end_ns = range.actual_end_ns;
  built.tracks = std::move(mux_tracks);
  built.packets = std::move(mux_packets);
  *plan = std::move(built);
  return true;
}

}  // namespace olouie::record
