#include "encode/H264PacketStore.h"

#include <span>
#include <string>
#include <utility>

namespace olouie::encode {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

bool H264PacketStoreConfig::IsReady() const noexcept {
  return track_id != 0 && packet_format != MfHardwareH264PacketFormat::Unknown &&
         !sps.empty() && !pps.empty() && !avcc_extradata.empty();
}

bool BuildH264PacketMetadata(uint32_t track_id,
                             const MfHardwareH264EncodedPacket& packet,
                             record::PacketMetadata* metadata,
                             std::wstring* error) noexcept {
  if (metadata == nullptr) {
    SetError(error, L"H.264 packet metadata needs an output destination.");
    return false;
  }

  *metadata = {};
  if (track_id == 0) {
    SetError(error, L"H.264 packet metadata needs a nonzero track id.");
    return false;
  }
  if (packet.data.empty()) {
    SetError(error, L"H.264 packet is missing encoded payload bytes.");
    return false;
  }
  if (packet.pts_ns < 0 || packet.duration_ns < 0) {
    SetError(error, L"H.264 packet timing must not be negative.");
    return false;
  }

  metadata->track_id = track_id;
  metadata->codec_id = record::CodecId::H264;
  metadata->flags = packet.keyframe ? record::PacketFlagKeyframe
                                    : record::PacketFlagNone;
  metadata->pts_ns = packet.pts_ns;
  metadata->dts_ns = packet.pts_ns;
  metadata->duration_ns = packet.duration_ns;
  return true;
}

bool BuildH264PacketStoreConfig(uint32_t track_id,
                                const MfHardwareH264ConfigRecord& config,
                                H264PacketStoreConfig* output,
                                std::wstring* error) {
  if (output == nullptr) {
    SetError(error, L"H.264 PacketStore config needs an output destination.");
    return false;
  }

  *output = {};
  if (track_id == 0) {
    SetError(error, L"H.264 PacketStore config needs a nonzero track id.");
    return false;
  }
  if (!config.HasAvccExtradata()) {
    SetError(error, L"H.264 PacketStore config needs AVCC extradata.");
    return false;
  }

  output->track_id = track_id;
  output->packet_format = config.packet_format;
  output->sps = config.sps;
  output->pps = config.pps;
  output->avcc_extradata = config.avcc_extradata;
  return true;
}

bool AppendH264Packet(record::PacketStore* store,
                      uint32_t track_id,
                      const MfHardwareH264EncodedPacket& packet,
                      std::wstring* error) {
  if (store == nullptr) {
    SetError(error, L"H.264 packet append needs a PacketStore.");
    return false;
  }

  record::PacketMetadata metadata;
  if (!BuildH264PacketMetadata(track_id, packet, &metadata, error)) {
    return false;
  }

  return store->AppendPacket(metadata, std::as_bytes(std::span(packet.data)),
                             error);
}

}  // namespace olouie::encode
