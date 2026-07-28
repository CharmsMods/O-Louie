#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "record/DiskWriteFault.h"

namespace olouie::record {

enum class CodecId : uint16_t {
  Unknown = 0,
  H264 = 1,
  Aac = 2,
};

enum PacketFlag : uint16_t {
  PacketFlagNone = 0,
  PacketFlagKeyframe = 1 << 0,
  PacketFlagConfig = 1 << 1,
  PacketFlagDiscontinuity = 1 << 2,
};

struct TrackDefinition {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
};

struct PacketMetadata {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  uint16_t flags = PacketFlagNone;
  int64_t pts_ns = 0;
  int64_t dts_ns = 0;
  int64_t duration_ns = 0;
};

struct PacketIndexEntry {
  uint64_t file_offset = 0;
  uint64_t packet_size = 0;
  uint64_t payload_size = 0;
  PacketMetadata metadata;

  bool IsKeyframe() const noexcept;
  int64_t EndPtsNs() const noexcept;
};

struct PacketRange {
  int64_t requested_start_ns = 0;
  int64_t requested_end_ns = 0;
  int64_t actual_start_ns = 0;
  int64_t actual_end_ns = 0;
  std::vector<PacketIndexEntry> packets;
};

struct PacketStoreExportSnapshot {
  std::filesystem::path session_dir;
  std::filesystem::path packet_file_path;
  std::vector<PacketIndexEntry> index;

  bool IsReady() const noexcept;
};

struct PacketStoreRecoveryInfo {
  uint64_t file_size = 0;
  uint64_t recovered_bytes = 0;
  uint64_t trailing_bytes = 0;
  uint64_t packet_count = 0;

  bool HasTruncatedTail() const noexcept;
};

struct PacketStoreTrackStats {
  uint32_t track_id = 0;
  CodecId codec_id = CodecId::Unknown;
  uint64_t packet_count = 0;
  uint64_t payload_byte_count = 0;
};

struct PacketStoreStats {
  uint64_t packet_count = 0;
  uint64_t payload_byte_count = 0;
  std::vector<PacketStoreTrackStats> tracks;
};

struct PacketStoreWriterOptions {
  size_t max_queued_packet_count = 512;
  size_t max_queued_payload_bytes = 32u * 1024u * 1024u;

  bool IsValid() const noexcept;
};

struct PacketStoreWriterStats {
  size_t queued_packet_count = 0;
  size_t peak_queued_packet_count = 0;
  size_t queued_payload_bytes = 0;
  size_t peak_queued_payload_bytes = 0;
  uint64_t enqueued_packet_count = 0;
  uint64_t persisted_packet_count = 0;
  uint64_t rejected_packet_count = 0;
  uint64_t flush_count = 0;
  uint64_t last_queue_latency_ns = 0;
  uint64_t maximum_queue_latency_ns = 0;
  uint64_t total_queue_latency_ns = 0;
  uint64_t last_write_latency_ns = 0;
  uint64_t maximum_write_latency_ns = 0;
  uint64_t total_write_latency_ns = 0;
};

PacketRange QueryPacketRange(
    std::span<const PacketIndexEntry> index,
    int64_t start_ns,
    int64_t end_ns,
    bool include_previous_keyframe);

class PacketStore final {
 public:
  PacketStore() = default;
  ~PacketStore();

  PacketStore(PacketStore&&) noexcept;
  PacketStore& operator=(PacketStore&&) noexcept;

  PacketStore(const PacketStore&) = delete;
  PacketStore& operator=(const PacketStore&) = delete;

  static PacketStore Create(const std::filesystem::path& session_dir,
                            std::span<const TrackDefinition> tracks,
                            std::wstring* error,
                            DiskWriteFault* write_fault = nullptr,
                            PacketStoreWriterOptions writer_options = {});
  static PacketStore Recover(const std::filesystem::path& session_dir,
                             std::wstring* error,
                             PacketStoreRecoveryInfo* recovery_info = nullptr);

  bool IsWritable() const noexcept;
  bool AppendPacket(const PacketMetadata& metadata,
                    std::span<const std::byte> payload, std::wstring* error);
  std::vector<PacketIndexEntry> SnapshotIndex() const;
  PacketStoreStats SnapshotStats() const;
  PacketStoreWriterStats SnapshotWriterStats() const;
  bool SnapshotForExport(PacketStoreExportSnapshot* snapshot,
                         std::wstring* error);
  PacketRange QueryRange(int64_t start_ns, int64_t end_ns,
                         bool include_previous_keyframe);
  bool ReadPayload(const PacketIndexEntry& entry,
                   std::vector<std::byte>* payload, std::wstring* error) const;
  bool Close(std::wstring* error = nullptr);

  DiskWriteFault last_write_fault() const;

  const std::filesystem::path& session_dir() const noexcept;
  const std::filesystem::path& packet_file_path() const noexcept;

 private:
  static PacketStore OpenForCreate(const std::filesystem::path& session_dir,
                                   std::span<const TrackDefinition> tracks,
                                   std::wstring* error,
                                   DiskWriteFault* write_fault,
                                   PacketStoreWriterOptions writer_options);

  bool ValidateTrack(const PacketMetadata& metadata, std::wstring* error) const;
  void AddIndexEntry(uint64_t file_offset, uint64_t packet_size,
                     uint64_t payload_size, const PacketMetadata& metadata);
  void LatchWriteFault(DiskWriteFault fault);
  void MoveFrom(PacketStore&& other) noexcept;

  mutable std::mutex mutex_;
  std::filesystem::path session_dir_;
  std::filesystem::path packet_file_path_;
  class AsyncPacketWriter;
  std::shared_ptr<AsyncPacketWriter> writer_;
  std::map<uint32_t, CodecId> tracks_;
  std::vector<PacketIndexEntry> index_;
  PacketStoreStats stats_;
  DiskWriteFault last_write_fault_;
};

}  // namespace olouie::record
