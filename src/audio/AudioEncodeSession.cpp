#include "audio/AudioEncodeSession.h"

#include <limits>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioEncodeSessionResult Result(AudioEncodeSessionStatus status,
                                std::wstring message,
                                uint32_t track_id,
                                size_t processed_block_count = 0) {
  AudioEncodeSessionResult result;
  result.status = status;
  result.message = std::move(message);
  result.track_id = track_id;
  result.processed_block_count = processed_block_count;
  return result;
}

bool HasDuplicateTrackId(std::span<const AudioEncodeSessionTrack> tracks,
                         uint32_t track_id,
                         size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (tracks[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool AudioEncodeSessionResult::Succeeded() const noexcept {
  return status == AudioEncodeSessionStatus::Success;
}

AudioEncodeSession::AudioEncodeSession(
    std::span<const AudioEncodeSessionTrack> tracks,
    record::PacketStore* packet_store) {
  if (packet_store == nullptr || tracks.empty()) {
    return;
  }

  for (size_t index = 0; index < tracks.size(); ++index) {
    const auto& track = tracks[index];
    if (track.track_id == 0 || track.encoder == nullptr ||
        !track.config.IsValid() ||
        HasDuplicateTrackId(tracks, track.track_id, index)) {
      tracks_.clear();
      return;
    }
  }

  tracks_.reserve(tracks.size());
  for (const auto& track : tracks) {
    TrackEntry entry;
    entry.track_id = track.track_id;
    entry.chain = std::make_unique<AudioTrackEncodeChain>(
        track.config, track.encoder, packet_store);
    tracks_.push_back(std::move(entry));
  }

  configured_ = true;
}

bool AudioEncodeSession::IsConfigured() const noexcept {
  return configured_;
}

size_t AudioEncodeSession::track_count() const noexcept {
  return tracks_.size();
}

bool AudioEncodeSession::HasTrack(uint32_t track_id) const {
  return FindTrack(track_id) != nullptr;
}

bool AudioEncodeSession::TryGetTrackInputFormat(
    uint32_t track_id,
    PcmStreamFormat* format) const noexcept {
  if (format == nullptr) {
    return false;
  }
  const auto* track = FindTrack(track_id);
  if (track == nullptr || !track->config().input_format.IsValid()) {
    return false;
  }
  *format = track->config().input_format;
  return true;
}

size_t AudioEncodeSession::QueuedBlockCount(uint32_t track_id) const {
  const auto* track = FindTrack(track_id);
  if (track == nullptr) {
    return 0;
  }

  return track->queued_block_count();
}

size_t AudioEncodeSession::TotalQueuedBlockCount() const {
  size_t total = 0;
  for (const auto& entry : tracks_) {
    total += entry.chain->queued_block_count();
  }
  return total;
}

AudioEncodeSessionResult AudioEncodeSession::QueueCapturedPcm(
    uint32_t track_id,
    const PcmStreamFormat& captured_format,
    const PcmPacketInfo& packet,
    int64_t pts_ns,
    std::span<const std::byte> pcm_bytes) {
  if (!configured_) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio encode session is not configured.", track_id);
  }

  auto* track = FindTrack(track_id);
  if (track == nullptr) {
    return Result(AudioEncodeSessionStatus::UnknownTrack,
                  L"Audio encode session has no matching track.", track_id);
  }

  const auto queued =
      track->QueueCapturedPcm(captured_format, packet, pts_ns, pcm_bytes);
  if (!queued.Succeeded()) {
    ++stats_.queue_failure_count;
    return Result(AudioEncodeSessionStatus::QueueError, queued.message,
                  track_id);
  }

  ++stats_.queued_packet_count;
  return Result(AudioEncodeSessionStatus::Success, L"", track_id);
}

AudioEncodeSessionResult AudioEncodeSession::DrainTrack(uint32_t track_id,
                                                        size_t max_blocks) {
  if (!configured_) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio encode session is not configured.", track_id);
  }

  auto* track = FindTrack(track_id);
  if (track == nullptr) {
    return Result(AudioEncodeSessionStatus::UnknownTrack,
                  L"Audio encode session has no matching track.", track_id);
  }

  const auto drained = track->DrainQueuedBlocks(max_blocks);
  if (!drained.Succeeded()) {
    ++stats_.drain_failure_count;
    stats_.drained_block_count += drained.submitted_block_count;
    return Result(AudioEncodeSessionStatus::DrainError, drained.message,
                  track_id, drained.submitted_block_count);
  }

  stats_.drained_block_count += drained.submitted_block_count;
  return Result(AudioEncodeSessionStatus::Success, L"", track_id,
                drained.submitted_block_count);
}

AudioEncodeSessionResult AudioEncodeSession::DrainQueuedBlocks(
    size_t max_total_blocks) {
  if (!configured_) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio encode session is not configured.", 0);
  }

  if (max_total_blocks == 0) {
    return Result(AudioEncodeSessionStatus::Success, L"", 0);
  }

  size_t total_drained = 0;
  while (total_drained < max_total_blocks) {
    bool drained_any_track = false;

    for (auto& entry : tracks_) {
      if (total_drained >= max_total_blocks) {
        break;
      }

      if (entry.chain->queue_empty()) {
        continue;
      }

      const auto drained = entry.chain->DrainQueuedBlocks(1);
      if (!drained.Succeeded()) {
        ++stats_.drain_failure_count;
        total_drained += drained.submitted_block_count;
        stats_.drained_block_count += total_drained;
        return Result(AudioEncodeSessionStatus::DrainError, drained.message,
                      entry.track_id, total_drained);
      }

      if (drained.submitted_block_count > 0) {
        total_drained += drained.submitted_block_count;
        drained_any_track = true;
      }
    }

    if (!drained_any_track) {
      break;
    }
  }

  stats_.drained_block_count += total_drained;
  return Result(AudioEncodeSessionStatus::Success, L"", 0, total_drained);
}

AudioEncodeSessionResult AudioEncodeSession::DrainAllQueuedBlocks() {
  return DrainQueuedBlocks(std::numeric_limits<size_t>::max());
}

AudioEncodeSessionResult AudioEncodeSession::FlushTrack(uint32_t track_id) {
  if (!configured_) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio encode session is not configured.", track_id);
  }

  auto* track = FindTrack(track_id);
  if (track == nullptr) {
    return Result(AudioEncodeSessionStatus::UnknownTrack,
                  L"Audio encode session has no matching track.", track_id);
  }

  const auto flushed = track->Flush();
  if (!flushed.Succeeded()) {
    ++stats_.flush_failure_count;
    return Result(AudioEncodeSessionStatus::FlushError, flushed.message,
                  track_id);
  }

  ++stats_.flushed_track_count;
  return Result(AudioEncodeSessionStatus::Success, L"", track_id);
}

AudioEncodeSessionResult AudioEncodeSession::FlushAllTracks() {
  if (!configured_) {
    return Result(AudioEncodeSessionStatus::InvalidConfig,
                  L"Audio encode session is not configured.", 0);
  }

  uint32_t flushed_count = 0;
  for (auto& entry : tracks_) {
    const auto flushed = entry.chain->Flush();
    if (!flushed.Succeeded()) {
      ++stats_.flush_failure_count;
      stats_.flushed_track_count += flushed_count;
      return Result(AudioEncodeSessionStatus::FlushError, flushed.message,
                    entry.track_id, flushed_count);
    }
    ++flushed_count;
  }

  stats_.flushed_track_count += flushed_count;
  return Result(AudioEncodeSessionStatus::Success, L"", 0, flushed_count);
}

const AudioEncodeSessionStats& AudioEncodeSession::stats() const noexcept {
  return stats_;
}

AudioTrackEncodeChain* AudioEncodeSession::FindTrack(
    uint32_t track_id) noexcept {
  for (auto& entry : tracks_) {
    if (entry.track_id == track_id) {
      return entry.chain.get();
    }
  }
  return nullptr;
}

const AudioTrackEncodeChain* AudioEncodeSession::FindTrack(
    uint32_t track_id) const noexcept {
  for (const auto& entry : tracks_) {
    if (entry.track_id == track_id) {
      return entry.chain.get();
    }
  }
  return nullptr;
}

}  // namespace olouie::audio
