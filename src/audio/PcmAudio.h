#pragma once

#include <cstdint>
#include <string>

#include "performance/MultimediaThreadScheduling.h"

namespace olouie::audio {

enum class PcmSampleEncoding {
  Unknown,
  UnsignedInteger,
  SignedInteger,
  Float,
};

struct PcmStreamFormat {
  uint32_t sample_rate = 0;
  uint16_t channel_count = 0;
  uint16_t bits_per_sample = 0;
  uint16_t block_align = 0;
  uint32_t average_bytes_per_second = 0;
  PcmSampleEncoding encoding = PcmSampleEncoding::Unknown;

  bool IsValid() const noexcept;
};

struct PcmPacketTiming {
  uint64_t device_position_frames = 0;
  uint64_t qpc_position_100ns = 0;
  uint32_t frame_count = 0;
  int64_t qpc_position_ns = 0;
  int64_t duration_ns = 0;

  bool IsValid() const noexcept;
};

struct PcmPacketInfo {
  PcmPacketTiming timing;
  bool silent = false;
  bool data_discontinuity = false;
  bool timestamp_error = false;
};

struct PcmCaptureStats {
  bool used_event_callback = false;
  performance::MultimediaThreadSchedulingSnapshot scheduling;
  PcmStreamFormat format;
  uint32_t packet_count = 0;
  uint64_t frame_count = 0;
  uint32_t silent_packet_count = 0;
  uint32_t data_discontinuity_count = 0;
  uint32_t timestamp_error_count = 0;
  uint32_t endpoint_invalidation_count = 0;
  uint32_t default_device_change_count = 0;
  uint32_t restart_attempt_count = 0;
  uint32_t restart_success_count = 0;
  uint32_t capture_format_change_count = 0;
  PcmPacketTiming first_packet;
  PcmPacketTiming last_packet;

  bool HasPackets() const noexcept;
  void AddPacket(const PcmPacketInfo& packet) noexcept;
};

PcmStreamFormat MakePcmStreamFormat(uint32_t sample_rate,
                                    uint16_t channel_count,
                                    uint16_t bits_per_sample,
                                    uint16_t block_align,
                                    uint32_t average_bytes_per_second,
                                    PcmSampleEncoding encoding) noexcept;

bool SamePcmStreamFormat(const PcmStreamFormat& left,
                         const PcmStreamFormat& right) noexcept;

bool BuildPcmPacketTiming(uint64_t device_position_frames,
                          uint64_t qpc_position_100ns,
                          uint32_t frame_count,
                          uint32_t sample_rate,
                          PcmPacketTiming* timing,
                          std::wstring* error) noexcept;

int64_t AudioFramesToNs(uint64_t frame_count, uint32_t sample_rate) noexcept;
int64_t Qpc100nsToNs(uint64_t qpc_position_100ns) noexcept;

}  // namespace olouie::audio
