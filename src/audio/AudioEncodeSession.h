#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio/AudioTrackEncodeChain.h"
#include "record/PacketStore.h"

namespace olouie::audio {

struct AudioEncodeSessionTrack {
  uint32_t track_id = 0;
  AudioTrackEncodeChainConfig config;
  IAacEncoder* encoder = nullptr;
};

enum class AudioEncodeSessionStatus {
  Success,
  InvalidConfig,
  UnknownTrack,
  QueueError,
  DrainError,
  FlushError,
};

struct AudioEncodeSessionResult {
  AudioEncodeSessionStatus status = AudioEncodeSessionStatus::InvalidConfig;
  std::wstring message;
  uint32_t track_id = 0;
  size_t processed_block_count = 0;

  bool Succeeded() const noexcept;
};

struct AudioEncodeSessionStats {
  uint64_t queued_packet_count = 0;
  uint64_t queue_failure_count = 0;
  uint64_t drained_block_count = 0;
  uint64_t drain_failure_count = 0;
  uint64_t flushed_track_count = 0;
  uint64_t flush_failure_count = 0;
};

class AudioEncodeSession final {
 public:
  AudioEncodeSession(std::span<const AudioEncodeSessionTrack> tracks,
                     record::PacketStore* packet_store);

  AudioEncodeSession(const AudioEncodeSession&) = delete;
  AudioEncodeSession& operator=(const AudioEncodeSession&) = delete;

  bool IsConfigured() const noexcept;
  size_t track_count() const noexcept;
  bool HasTrack(uint32_t track_id) const;
  bool TryGetTrackInputFormat(uint32_t track_id,
                              PcmStreamFormat* format) const noexcept;
  size_t QueuedBlockCount(uint32_t track_id) const;
  size_t TotalQueuedBlockCount() const;

  AudioEncodeSessionResult QueueCapturedPcm(
      uint32_t track_id,
      const PcmStreamFormat& captured_format,
      const PcmPacketInfo& packet,
      int64_t pts_ns,
      std::span<const std::byte> pcm_bytes);
  AudioEncodeSessionResult DrainTrack(uint32_t track_id, size_t max_blocks);
  AudioEncodeSessionResult DrainQueuedBlocks(size_t max_total_blocks);
  AudioEncodeSessionResult DrainAllQueuedBlocks();
  AudioEncodeSessionResult FlushTrack(uint32_t track_id);
  AudioEncodeSessionResult FlushAllTracks();

  const AudioEncodeSessionStats& stats() const noexcept;

 private:
  struct TrackEntry {
    uint32_t track_id = 0;
    std::unique_ptr<AudioTrackEncodeChain> chain;
  };

  AudioTrackEncodeChain* FindTrack(uint32_t track_id) noexcept;
  const AudioTrackEncodeChain* FindTrack(uint32_t track_id) const noexcept;

  bool configured_ = false;
  std::vector<TrackEntry> tracks_;
  AudioEncodeSessionStats stats_;
};

}  // namespace olouie::audio
