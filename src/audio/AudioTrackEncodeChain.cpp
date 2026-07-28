#include "audio/AudioTrackEncodeChain.h"

namespace olouie::audio {

bool AudioTrackEncodeChainConfig::IsValid() const noexcept {
  return input_format.IsValid() && output_sample_rate > 0 &&
         queue_capacity > 0;
}

AudioTrackEncodeChain::AudioTrackEncodeChain(
    AudioTrackEncodeChainConfig config,
    IAacEncoder* encoder,
    record::PacketStore* packet_store)
    : config_(config),
      queue_(config.queue_capacity, config.overflow_policy),
      queue_writer_(&queue_, config.input_format, config.output_sample_rate),
      sink_(encoder, packet_store),
      worker_(&queue_, &sink_) {}

CapturedPcmQueueResult AudioTrackEncodeChain::QueueCapturedPcm(
    const PcmStreamFormat& captured_format,
    const PcmPacketInfo& packet,
    int64_t pts_ns,
    std::span<const std::byte> pcm_bytes) {
  return queue_writer_.QueueCapturedPcm(captured_format, packet, pts_ns,
                                       pcm_bytes);
}

AudioEncodeWorkerResult AudioTrackEncodeChain::DrainQueuedBlocks(
    size_t max_blocks) {
  return worker_.DrainQueuedBlocks(max_blocks);
}

AudioEncodeWorkerResult AudioTrackEncodeChain::DrainAllQueuedBlocks() {
  return worker_.DrainAllQueuedBlocks();
}

AacEncodeSinkResult AudioTrackEncodeChain::Flush() {
  return sink_.Flush();
}

bool AudioTrackEncodeChain::IsConfigured() const noexcept {
  return config_.IsValid();
}

size_t AudioTrackEncodeChain::queued_block_count() const {
  return queue_.size();
}

bool AudioTrackEncodeChain::queue_empty() const {
  return queue_.empty();
}

const AudioTrackEncodeChainConfig& AudioTrackEncodeChain::config()
    const noexcept {
  return config_;
}

const CapturedPcmQueueStats& AudioTrackEncodeChain::queue_writer_stats()
    const noexcept {
  return queue_writer_.stats();
}

PreparedPcmQueueStats AudioTrackEncodeChain::queue_stats() const {
  return queue_.SnapshotStats();
}

const AudioEncodeWorkerStats& AudioTrackEncodeChain::worker_stats()
    const noexcept {
  return worker_.stats();
}

const AacEncodeSinkStats& AudioTrackEncodeChain::sink_stats() const noexcept {
  return sink_.stats();
}

}  // namespace olouie::audio
