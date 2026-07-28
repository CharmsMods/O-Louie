#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "record/PacketStore.h"

namespace olouie::record {

struct MuxTrack {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
};

struct MuxPacketRef {
  PacketIndexEntry packet;
  int64_t output_pts_ns = 0;
  int64_t output_dts_ns = 0;
  int64_t duration_ns = 0;
};

struct MuxPlan {
  int64_t source_start_ns = 0;
  int64_t source_end_ns = 0;
  std::vector<MuxTrack> tracks;
  std::vector<MuxPacketRef> packets;

  bool HasVideoTrack() const noexcept;
};

struct MuxPlanOptions {
  bool require_video_track = true;
  bool normalize_timestamps = true;
};

bool BuildMuxPlan(const PacketRange& range,
                  std::span<const TrackDefinition> track_definitions,
                  const MuxPlanOptions& options, MuxPlan* plan,
                  std::wstring* error);

}  // namespace olouie::record
