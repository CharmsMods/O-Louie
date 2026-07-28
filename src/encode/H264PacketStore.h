#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "encode/MfHardwareH264EncoderSession.h"
#include "record/PacketStore.h"

namespace olouie::encode {

struct H264PacketStoreConfig {
  uint32_t track_id = 0;
  MfHardwareH264PacketFormat packet_format = MfHardwareH264PacketFormat::Unknown;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  std::vector<uint8_t> avcc_extradata;

  bool IsReady() const noexcept;
};

bool BuildH264PacketMetadata(uint32_t track_id,
                             const MfHardwareH264EncodedPacket& packet,
                             record::PacketMetadata* metadata,
                             std::wstring* error) noexcept;

bool BuildH264PacketStoreConfig(uint32_t track_id,
                                const MfHardwareH264ConfigRecord& config,
                                H264PacketStoreConfig* output,
                                std::wstring* error);

bool AppendH264Packet(record::PacketStore* store,
                      uint32_t track_id,
                      const MfHardwareH264EncodedPacket& packet,
                      std::wstring* error);

}  // namespace olouie::encode
