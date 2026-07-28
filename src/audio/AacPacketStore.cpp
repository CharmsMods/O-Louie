#include "audio/AacPacketStore.h"

#include <span>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

bool BuildAacPacketMetadata(const EncodedAacPacket& packet,
                            record::PacketMetadata* metadata,
                            std::wstring* error) noexcept {
  if (metadata == nullptr) {
    SetError(error, L"AAC packet metadata needs an output destination.");
    return false;
  }

  *metadata = {};
  if (!packet.IsValid()) {
    SetError(error, L"AAC packet is missing required metadata or payload.");
    return false;
  }

  metadata->track_id = packet.track_id;
  metadata->codec_id = record::CodecId::Aac;
  metadata->flags = record::PacketFlagNone;
  metadata->pts_ns = packet.pts_ns;
  metadata->dts_ns = packet.dts_ns;
  metadata->duration_ns = packet.duration_ns;
  return true;
}

bool AppendAacPacket(record::PacketStore* store,
                     const EncodedAacPacket& packet,
                     std::wstring* error) {
  if (store == nullptr) {
    SetError(error, L"AAC packet append needs a PacketStore.");
    return false;
  }

  record::PacketMetadata metadata;
  if (!BuildAacPacketMetadata(packet, &metadata, error)) {
    return false;
  }

  return store->AppendPacket(metadata, std::span<const std::byte>(packet.data),
                             error);
}

}  // namespace olouie::audio
