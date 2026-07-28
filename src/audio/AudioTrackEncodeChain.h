#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "audio/AacEncodeSink.h"
#include "audio/AudioEncodeWorker.h"
#include "audio/CapturedPcmQueueWriter.h"
#include "audio/PreparedPcmQueue.h"

namespace olouie::audio {

struct AudioTrackEncodeChainConfig {
  PcmStreamFormat input_format;
  uint32_t output_sample_rate = 48000;
  size_t queue_capacity = 0;
  PreparedPcmOverflowPolicy overflow_policy =
      PreparedPcmOverflowPolicy::RejectNewest;

  bool IsValid() const noexcept;
};

class AudioTrackEncodeChain final {
 public:
  AudioTrackEncodeChain(AudioTrackEncodeChainConfig config,
                        IAacEncoder* encoder,
                        record::PacketStore* packet_store);

  AudioTrackEncodeChain(const AudioTrackEncodeChain&) = delete;
  AudioTrackEncodeChain& operator=(const AudioTrackEncodeChain&) = delete;

  CapturedPcmQueueResult QueueCapturedPcm(
      const PcmStreamFormat& captured_format,
      const PcmPacketInfo& packet,
      int64_t pts_ns,
      std::span<const std::byte> pcm_bytes);
  AudioEncodeWorkerResult DrainQueuedBlocks(size_t max_blocks);
  AudioEncodeWorkerResult DrainAllQueuedBlocks();
  AacEncodeSinkResult Flush();

  bool IsConfigured() const noexcept;
  size_t queued_block_count() const;
  bool queue_empty() const;

  const AudioTrackEncodeChainConfig& config() const noexcept;
  const CapturedPcmQueueStats& queue_writer_stats() const noexcept;
  PreparedPcmQueueStats queue_stats() const;
  const AudioEncodeWorkerStats& worker_stats() const noexcept;
  const AacEncodeSinkStats& sink_stats() const noexcept;

 private:
  AudioTrackEncodeChainConfig config_;
  PreparedPcmQueue queue_;
  CapturedPcmQueueWriter queue_writer_;
  AacEncodeSink sink_;
  AudioEncodeWorker worker_;
};

}  // namespace olouie::audio
