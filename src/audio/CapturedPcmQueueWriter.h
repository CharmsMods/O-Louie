#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "audio/AudioResampler.h"
#include "audio/PcmAudio.h"
#include "audio/PreparedPcmQueue.h"

namespace olouie::audio {

enum class CapturedPcmQueueStatus {
  Success,
  InvalidConfig,
  InvalidPacket,
  PrepareFailed,
  QueueRejected,
};

struct CapturedPcmQueueResult {
  CapturedPcmQueueStatus status = CapturedPcmQueueStatus::InvalidConfig;
  std::wstring message;
  uint32_t prepared_frame_count = 0;
  size_t queued_byte_count = 0;

  bool Succeeded() const noexcept;
};

struct CapturedPcmQueueStats {
  uint64_t attempted_packet_count = 0;
  uint64_t silent_packet_count = 0;
  uint64_t prepared_block_count = 0;
  uint64_t queued_block_count = 0;
  uint64_t queued_frame_count = 0;
  uint64_t resampled_packet_count = 0;
  uint64_t resampled_frame_count = 0;
  uint64_t input_format_change_count = 0;
  uint64_t prepare_failure_count = 0;
  uint64_t queue_rejection_count = 0;
};

class CapturedPcmQueueWriter final {
 public:
  CapturedPcmQueueWriter(PreparedPcmQueue* queue,
                         PcmStreamFormat input_format,
                         uint32_t output_sample_rate);

  CapturedPcmQueueResult QueueCapturedPcm(
      const PcmStreamFormat& captured_format,
      const PcmPacketInfo& packet,
      int64_t pts_ns,
      std::span<const std::byte> pcm_bytes);

  const CapturedPcmQueueStats& stats() const noexcept;

 private:
  PreparedPcmQueue* queue_ = nullptr;
  PcmStreamFormat input_format_;
  uint32_t output_sample_rate_ = 0;
  std::unique_ptr<StreamingPcmResampler> resampler_;
  CapturedPcmQueueStats stats_;
};

}  // namespace olouie::audio
