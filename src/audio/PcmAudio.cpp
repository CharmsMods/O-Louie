#include "audio/PcmAudio.h"

#include <limits>
#include <string>

namespace olouie::audio {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

bool PcmStreamFormat::IsValid() const noexcept {
  return sample_rate > 0 && channel_count > 0 && bits_per_sample > 0 &&
         block_align > 0 && average_bytes_per_second > 0 &&
         encoding != PcmSampleEncoding::Unknown;
}

bool PcmPacketTiming::IsValid() const noexcept {
  return frame_count > 0 && duration_ns > 0;
}

bool PcmCaptureStats::HasPackets() const noexcept {
  return packet_count > 0;
}

void PcmCaptureStats::AddPacket(const PcmPacketInfo& packet) noexcept {
  ++packet_count;
  frame_count += packet.timing.frame_count;
  if (packet.silent) {
    ++silent_packet_count;
  }
  if (packet.data_discontinuity) {
    ++data_discontinuity_count;
  }
  if (packet.timestamp_error) {
    ++timestamp_error_count;
  }

  if (packet_count == 1) {
    first_packet = packet.timing;
  }
  last_packet = packet.timing;
}

PcmStreamFormat MakePcmStreamFormat(uint32_t sample_rate,
                                    uint16_t channel_count,
                                    uint16_t bits_per_sample,
                                    uint16_t block_align,
                                    uint32_t average_bytes_per_second,
                                    PcmSampleEncoding encoding) noexcept {
  PcmStreamFormat format;
  format.sample_rate = sample_rate;
  format.channel_count = channel_count;
  format.bits_per_sample = bits_per_sample;
  format.block_align = block_align;
  format.average_bytes_per_second = average_bytes_per_second;
  format.encoding = encoding;
  return format;
}

bool SamePcmStreamFormat(const PcmStreamFormat& left,
                         const PcmStreamFormat& right) noexcept {
  return left.sample_rate == right.sample_rate &&
         left.channel_count == right.channel_count &&
         left.bits_per_sample == right.bits_per_sample &&
         left.block_align == right.block_align &&
         left.average_bytes_per_second == right.average_bytes_per_second &&
         left.encoding == right.encoding;
}

bool BuildPcmPacketTiming(uint64_t device_position_frames,
                          uint64_t qpc_position_100ns,
                          uint32_t frame_count,
                          uint32_t sample_rate,
                          PcmPacketTiming* timing,
                          std::wstring* error) noexcept {
  if (timing == nullptr) {
    SetError(error, L"PCM packet timing needs an output destination.");
    return false;
  }

  if (frame_count == 0) {
    SetError(error, L"PCM packet timing needs at least one frame.");
    return false;
  }

  if (sample_rate == 0) {
    SetError(error, L"PCM packet timing needs a positive sample rate.");
    return false;
  }

  PcmPacketTiming built;
  built.device_position_frames = device_position_frames;
  built.qpc_position_100ns = qpc_position_100ns;
  built.frame_count = frame_count;
  built.qpc_position_ns = Qpc100nsToNs(qpc_position_100ns);
  built.duration_ns = AudioFramesToNs(frame_count, sample_rate);
  *timing = built;
  return true;
}

int64_t AudioFramesToNs(uint64_t frame_count, uint32_t sample_rate) noexcept {
  if (sample_rate == 0) {
    return 0;
  }

  const long double ns =
      (static_cast<long double>(frame_count) * 1000000000.0L) /
      static_cast<long double>(sample_rate);
  if (ns > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(ns + 0.5L);
}

int64_t Qpc100nsToNs(uint64_t qpc_position_100ns) noexcept {
  if (qpc_position_100ns >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 100)) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(qpc_position_100ns * 100);
}

}  // namespace olouie::audio
