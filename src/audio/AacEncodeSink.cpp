#include "audio/AacEncodeSink.h"

#include <string>
#include <utility>
#include <vector>

#include "audio/AacPacketStore.h"

namespace olouie::audio {
namespace {

AacEncodeSinkResult Result(AacEncodeSinkStatus status, std::wstring message) {
  AacEncodeSinkResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

}  // namespace

bool AacEncodeSinkResult::Succeeded() const noexcept {
  return status == AacEncodeSinkStatus::Success;
}

AacEncodeSink::AacEncodeSink(IAacEncoder* encoder,
                             record::PacketStore* packet_store)
    : encoder_(encoder), packet_store_(packet_store) {}

AacEncodeSinkResult AacEncodeSink::SubmitPreparedPcm(
    const AacPcmInput& input, std::span<const std::byte> pcm_bytes) {
  if (encoder_ == nullptr || packet_store_ == nullptr) {
    return Result(AacEncodeSinkStatus::InvalidArgument,
                  L"AAC encode sink needs an encoder and PacketStore.");
  }

  const auto submit = encoder_->SubmitPcm(input, pcm_bytes);
  if (!submit.Succeeded()) {
    return Result(AacEncodeSinkStatus::EncoderError, submit.message);
  }

  ++stats_.submitted_block_count;
  stats_.submitted_frame_count += input.frame_count;
  return DrainAndAppend();
}

AacEncodeSinkResult AacEncodeSink::DrainAvailable() {
  if (encoder_ == nullptr || packet_store_ == nullptr) {
    return Result(AacEncodeSinkStatus::InvalidArgument,
                  L"AAC encode sink needs an encoder and PacketStore.");
  }

  return DrainAndAppend();
}

AacEncodeSinkResult AacEncodeSink::Flush() {
  if (encoder_ == nullptr || packet_store_ == nullptr) {
    return Result(AacEncodeSinkStatus::InvalidArgument,
                  L"AAC encode sink needs an encoder and PacketStore.");
  }

  std::vector<EncodedAacPacket> packets;
  const auto flush = encoder_->Flush(&packets);
  if (!flush.Succeeded()) {
    return Result(AacEncodeSinkStatus::EncoderError, flush.message);
  }

  return AppendPackets(packets);
}

const AacEncodeSinkStats& AacEncodeSink::stats() const noexcept {
  return stats_;
}

AacEncodeSinkResult AacEncodeSink::AppendPackets(
    const std::vector<EncodedAacPacket>& packets) {
  std::wstring error;
  for (const auto& packet : packets) {
    ++stats_.drained_packet_count;
    if (!AppendAacPacket(packet_store_, packet, &error)) {
      return Result(AacEncodeSinkStatus::PacketStoreError, std::move(error));
    }
    ++stats_.appended_packet_count;
  }

  return Result(AacEncodeSinkStatus::Success, L"");
}

AacEncodeSinkResult AacEncodeSink::DrainAndAppend() {
  std::vector<EncodedAacPacket> packets;
  const auto drain = encoder_->DrainAvailable(&packets);
  if (!drain.Succeeded()) {
    return Result(AacEncodeSinkStatus::EncoderError, drain.message);
  }

  return AppendPackets(packets);
}

}  // namespace olouie::audio
