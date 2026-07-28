#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/AacEncoder.h"
#include "record/PacketStore.h"

namespace olouie::audio {

enum class AacEncodeSinkStatus {
  Success,
  InvalidArgument,
  EncoderError,
  PacketStoreError,
};

struct AacEncodeSinkResult {
  AacEncodeSinkStatus status = AacEncodeSinkStatus::InvalidArgument;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct AacEncodeSinkStats {
  uint64_t submitted_block_count = 0;
  uint64_t submitted_frame_count = 0;
  uint64_t drained_packet_count = 0;
  uint64_t appended_packet_count = 0;
};

class AacEncodeSink final {
 public:
  AacEncodeSink(IAacEncoder* encoder, record::PacketStore* packet_store);

  AacEncodeSinkResult SubmitPreparedPcm(const AacPcmInput& input,
                                        std::span<const std::byte> pcm_bytes);
  AacEncodeSinkResult DrainAvailable();
  AacEncodeSinkResult Flush();

  const AacEncodeSinkStats& stats() const noexcept;

 private:
  AacEncodeSinkResult AppendPackets(
      const std::vector<EncodedAacPacket>& packets);
  AacEncodeSinkResult DrainAndAppend();

  IAacEncoder* encoder_ = nullptr;
  record::PacketStore* packet_store_ = nullptr;
  AacEncodeSinkStats stats_;
};

}  // namespace olouie::audio
