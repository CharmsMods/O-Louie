#include "record/PacketStore.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace olouie::record {
namespace {

constexpr uint32_t kPacketMagic = 0x50475244;  // DRGP
constexpr uint16_t kPacketVersion = 1;

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool RangesOverlap(int64_t packet_start, int64_t packet_end,
                   int64_t range_start, int64_t range_end) {
  return packet_start < range_end && packet_end > range_start;
}

bool IsKnownCodec(CodecId codec_id) {
  return codec_id == CodecId::H264 || codec_id == CodecId::Aac;
}

std::error_code StreamErrorCode() {
  if (errno != 0) {
    return {errno, std::generic_category()};
  }
  return std::make_error_code(std::errc::io_error);
}

}  // namespace

struct DiskPacketHeader {
  uint32_t magic = kPacketMagic;
  uint16_t version = kPacketVersion;
  uint16_t header_size = sizeof(DiskPacketHeader);
  uint32_t track_id = 0;
  uint16_t codec_id = 0;
  uint16_t flags = 0;
  int64_t pts_ns = 0;
  int64_t dts_ns = 0;
  int64_t duration_ns = 0;
  uint64_t payload_size = 0;
};

static_assert(sizeof(DiskPacketHeader) == 48);

class PacketStore::AsyncPacketWriter final {
 public:
  AsyncPacketWriter(std::filesystem::path packet_file_path,
                    std::span<const TrackDefinition> tracks,
                    PacketStoreWriterOptions options)
      : packet_file_path_(std::move(packet_file_path)), options_(options) {
    for (const auto& track : tracks) {
      logical_stats_.tracks.push_back(
          PacketStoreTrackStats{track.track_id, track.codec_id});
    }
  }

  ~AsyncPacketWriter() {
    std::wstring ignored;
    (void)Close(&ignored);
  }

  bool Start(std::wstring* error, DiskWriteFault* write_fault) {
    if (!options_.IsValid()) {
      SetError(error, L"Packet writer queue limits must be nonzero.");
      return false;
    }

    errno = 0;
    stream_.open(packet_file_path_,
                 std::ios::binary | std::ios::trunc | std::ios::out);
    if (!stream_.is_open()) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::OpenFile,
          packet_file_path_, StreamErrorCode());
      {
        std::lock_guard lock(mutex_);
        LatchFaultLocked(fault, true);
      }
      if (write_fault != nullptr) {
        *write_fault = fault;
      }
      SetError(error, DescribeDiskWriteFault(fault));
      return false;
    }

    {
      std::lock_guard lock(mutex_);
      open_ = true;
      accepting_ = true;
    }
    try {
      thread_ = std::thread(&AsyncPacketWriter::Run, this);
    } catch (const std::exception&) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::OpenFile,
          packet_file_path_, std::make_error_code(std::errc::resource_unavailable_try_again),
          0, L"Could not start the asynchronous packet writer thread.");
      {
        std::lock_guard lock(mutex_);
        LatchFaultLocked(fault, true);
        open_ = false;
      }
      stream_.close();
      if (write_fault != nullptr) {
        *write_fault = fault;
      }
      SetError(error, DescribeDiskWriteFault(fault));
      return false;
    }
    return true;
  }

  bool Enqueue(const PacketMetadata& metadata,
               std::span<const std::byte> payload, std::wstring* error) {
    if (payload.size() >
        static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
      SetError(error, L"Packet payload is too large to buffer or write.");
      return false;
    }
    QueuedPacket packet;
    packet.metadata = metadata;
    packet.enqueued_at = std::chrono::steady_clock::now();
    try {
      packet.payload.assign(payload.begin(), payload.end());
    } catch (const std::exception&) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
          packet_file_path_, std::make_error_code(std::errc::not_enough_memory),
          0, L"Could not buffer an encoded packet for writing.");
      {
        std::lock_guard lock(mutex_);
        ++writer_stats_.rejected_packet_count;
        LatchFaultLocked(fault, false);
      }
      SetError(error, DescribeDiskWriteFault(fault));
      return false;
    }

    {
      std::lock_guard lock(mutex_);
      if (!accepting_ || !open_) {
        SetError(error, last_write_fault_.Failed()
                            ? DescribeDiskWriteFault(last_write_fault_)
                            : L"Packet writer is not accepting packets.");
        return false;
      }
      const bool packet_limit_reached =
          writer_stats_.queued_packet_count >=
          options_.max_queued_packet_count;
      const bool payload_limit_reached =
          packet.payload.size() > options_.max_queued_payload_bytes ||
          writer_stats_.queued_payload_bytes >
              options_.max_queued_payload_bytes - packet.payload.size();
      if (packet_limit_reached || payload_limit_reached) {
        ++writer_stats_.rejected_packet_count;
        const auto fault = MakeDiskWriteFault(
            DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
            packet_file_path_, std::make_error_code(std::errc::no_buffer_space),
            0,
            L"The bounded packet writer queue filled before storage caught up.");
        LatchFaultLocked(fault, false);
        SetError(error, DescribeDiskWriteFault(last_write_fault_));
        return false;
      }

      WorkItem item;
      item.kind = WorkKind::Packet;
      item.packet = std::move(packet);
      try {
        work_.push_back(std::move(item));
      } catch (const std::exception&) {
        ++writer_stats_.rejected_packet_count;
        const auto fault = MakeDiskWriteFault(
            DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
            packet_file_path_,
            std::make_error_code(std::errc::not_enough_memory), 0,
            L"Could not enqueue an encoded packet for asynchronous writing.");
        LatchFaultLocked(fault, false);
        SetError(error, DescribeDiskWriteFault(last_write_fault_));
        return false;
      }
      writer_stats_.queued_packet_count++;
      writer_stats_.queued_payload_bytes += payload.size();
      writer_stats_.peak_queued_packet_count = std::max(
          writer_stats_.peak_queued_packet_count,
          writer_stats_.queued_packet_count);
      writer_stats_.peak_queued_payload_bytes = std::max(
          writer_stats_.peak_queued_payload_bytes,
          writer_stats_.queued_payload_bytes);
      ++writer_stats_.enqueued_packet_count;
      AddLogicalStats(metadata, payload.size());
    }
    ready_.notify_one();
    return true;
  }

  bool Flush(std::wstring* error) {
    std::shared_ptr<Completion> waiter;
    try {
      waiter = std::make_shared<Completion>();
    } catch (const std::exception&) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Flush,
          packet_file_path_, std::make_error_code(std::errc::not_enough_memory),
          0, L"Could not allocate a packet writer flush barrier.");
      {
        std::lock_guard lock(mutex_);
        LatchFaultLocked(fault, false);
      }
      SetError(error, DescribeDiskWriteFault(fault));
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable()) {
        if (last_write_fault_.Failed()) {
          SetError(error, DescribeDiskWriteFault(last_write_fault_));
          return false;
        }
        return true;
      }
      if (closing_) {
        SetError(error, L"Packet writer is already closing.");
        return false;
      }
      WorkItem item;
      item.kind = WorkKind::Flush;
      item.completion = waiter;
      try {
        work_.push_back(std::move(item));
      } catch (const std::exception&) {
        const auto fault = MakeDiskWriteFault(
            DiskWriteSubsystem::PacketStore, DiskWriteOperation::Flush,
            packet_file_path_,
            std::make_error_code(std::errc::not_enough_memory), 0,
            L"Could not enqueue a packet writer flush barrier.");
        LatchFaultLocked(fault, false);
        SetError(error, DescribeDiskWriteFault(last_write_fault_));
        return false;
      }
    }
    ready_.notify_one();
    return WaitForCompletion(waiter, error);
  }

  bool Close(std::wstring* error) {
    std::unique_lock close_lock(close_mutex_);
    {
      std::lock_guard lock(mutex_);
      accepting_ = false;
      if (!thread_.joinable()) {
        if (stream_.is_open()) {
          stream_.close();
        }
        if (last_write_fault_.Failed()) {
          SetError(error, DescribeDiskWriteFault(last_write_fault_));
          return false;
        }
        return true;
      }
      closing_ = true;
    }

    std::shared_ptr<Completion> waiter;
    try {
      waiter = std::make_shared<Completion>();
    } catch (const std::exception&) {
    }
    {
      std::lock_guard lock(mutex_);
      WorkItem item;
      item.kind = WorkKind::Close;
      item.completion = waiter;
      if (waiter != nullptr) {
        try {
          work_.push_back(std::move(item));
        } catch (const std::exception&) {
          waiter.reset();
        }
      }
      if (waiter == nullptr) {
        const auto fault = MakeDiskWriteFault(
            DiskWriteSubsystem::PacketStore, DiskWriteOperation::Close,
            packet_file_path_,
            std::make_error_code(std::errc::not_enough_memory), 0,
            L"Could not enqueue the packet writer close barrier; queued "
            L"packets will still be drained.");
        LatchFaultLocked(fault, false);
        close_when_drained_ = true;
      }
    }
    ready_.notify_one();
    const bool completed =
        waiter != nullptr ? WaitForCompletion(waiter, error) : false;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (waiter == nullptr) {
      SetError(error, DescribeDiskWriteFault(last_write_fault()));
    }
    return completed;
  }

  bool IsWritable() const noexcept {
    std::lock_guard lock(mutex_);
    return accepting_ && open_ && !last_write_fault_.Failed();
  }

  std::vector<PacketIndexEntry> SnapshotIndex() const {
    std::lock_guard lock(mutex_);
    return persisted_index_;
  }

  PacketStoreStats SnapshotStats() const {
    std::lock_guard lock(mutex_);
    return logical_stats_;
  }

  PacketStoreWriterStats SnapshotWriterStats() const {
    std::lock_guard lock(mutex_);
    return writer_stats_;
  }

  DiskWriteFault last_write_fault() const {
    std::lock_guard lock(mutex_);
    return last_write_fault_;
  }

 private:
  enum class WorkKind { Packet, Flush, Close };

  struct QueuedPacket {
    PacketMetadata metadata;
    std::vector<std::byte> payload;
    std::chrono::steady_clock::time_point enqueued_at;
  };

  struct Completion {
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    bool succeeded = false;
    DiskWriteFault fault;
  };

  struct WorkItem {
    WorkKind kind = WorkKind::Packet;
    QueuedPacket packet;
    std::shared_ptr<Completion> completion;
  };

  static uint64_t NanosecondsBetween(
      std::chrono::steady_clock::time_point start,
      std::chrono::steady_clock::time_point end) noexcept {
    if (start == std::chrono::steady_clock::time_point{} || end < start) {
      return 0;
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }

  void Run() {
    for (;;) {
      WorkItem item;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] {
          return !work_.empty() || close_when_drained_;
        });
        if (work_.empty()) {
          item.kind = WorkKind::Close;
        } else {
          item = std::move(work_.front());
          work_.pop_front();
        }
        if (item.kind == WorkKind::Packet) {
          --writer_stats_.queued_packet_count;
          writer_stats_.queued_payload_bytes -= item.packet.payload.size();
        }
      }

      if (item.kind == WorkKind::Packet) {
        bool io_failed = false;
        {
          std::lock_guard lock(mutex_);
          io_failed = io_failed_;
        }
        if (!io_failed) {
          (void)WritePacket(item.packet);
        }
        continue;
      }

      bool stream_succeeded = true;
      {
        std::lock_guard lock(mutex_);
        stream_succeeded = !io_failed_;
      }
      if (stream_succeeded) {
        stream_succeeded = FlushStream(
            item.kind == WorkKind::Close
                ? L"The final packet data could not be made durable."
                : L"The active store could not be made visible to an export.");
      }
      if (item.kind == WorkKind::Close) {
        errno = 0;
        stream_.close();
        if (stream_.fail()) {
          const auto fault = MakeDiskWriteFault(
              DiskWriteSubsystem::PacketStore, DiskWriteOperation::Close,
              packet_file_path_, StreamErrorCode());
          std::lock_guard lock(mutex_);
          LatchFaultLocked(fault, true);
          stream_succeeded = false;
        }
        {
          std::lock_guard lock(mutex_);
          open_ = false;
        }
      }

      Complete(item.completion, stream_succeeded);
      if (item.kind == WorkKind::Close) {
        break;
      }
    }
  }

  bool WritePacket(const QueuedPacket& packet) {
    const auto write_started = std::chrono::steady_clock::now();
    errno = 0;
    const auto offset = stream_.tellp();
    if (offset < 0) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
          packet_file_path_, StreamErrorCode(), 0,
          L"Could not determine the packet append offset.");
      std::lock_guard lock(mutex_);
      LatchFaultLocked(fault, true);
      return false;
    }

    DiskPacketHeader header;
    header.track_id = packet.metadata.track_id;
    header.codec_id = static_cast<uint16_t>(packet.metadata.codec_id);
    header.flags = packet.metadata.flags;
    header.pts_ns = packet.metadata.pts_ns;
    header.dts_ns = packet.metadata.dts_ns;
    header.duration_ns = packet.metadata.duration_ns;
    header.payload_size = packet.payload.size();

    stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!packet.payload.empty()) {
      stream_.write(reinterpret_cast<const char*>(packet.payload.data()),
                    static_cast<std::streamsize>(packet.payload.size()));
    }
    if (!stream_.good()) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
          packet_file_path_, StreamErrorCode(), 0,
          L"The encoded packet could not be persisted.");
      std::lock_guard lock(mutex_);
      LatchFaultLocked(fault, true);
      return false;
    }

    const bool durable_boundary =
        (packet.metadata.flags &
         (PacketFlagKeyframe | PacketFlagConfig | PacketFlagDiscontinuity)) !=
        0;
    if (durable_boundary) {
      errno = 0;
      stream_.flush();
    }
    const auto write_finished = std::chrono::steady_clock::now();

    if (!stream_.good()) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Flush,
          packet_file_path_, StreamErrorCode(), 0,
          L"A key/config/discontinuity packet boundary could not be made "
          L"durable.");
      std::lock_guard lock(mutex_);
      LatchFaultLocked(fault, true);
      return false;
    }

    PacketIndexEntry entry;
    entry.file_offset = static_cast<uint64_t>(offset);
    entry.packet_size = sizeof(DiskPacketHeader) + packet.payload.size();
    entry.payload_size = packet.payload.size();
    entry.metadata = packet.metadata;
    const auto queue_latency =
        NanosecondsBetween(packet.enqueued_at, write_started);
    const auto write_latency =
        NanosecondsBetween(write_started, write_finished);
    {
      std::lock_guard lock(mutex_);
      persisted_index_.push_back(std::move(entry));
      ++writer_stats_.persisted_packet_count;
      writer_stats_.last_queue_latency_ns = queue_latency;
      writer_stats_.maximum_queue_latency_ns = std::max(
          writer_stats_.maximum_queue_latency_ns, queue_latency);
      writer_stats_.total_queue_latency_ns += queue_latency;
      writer_stats_.last_write_latency_ns = write_latency;
      writer_stats_.maximum_write_latency_ns = std::max(
          writer_stats_.maximum_write_latency_ns, write_latency);
      writer_stats_.total_write_latency_ns += write_latency;
      if (durable_boundary) {
        ++writer_stats_.flush_count;
      }
    }
    return true;
  }

  bool FlushStream(std::wstring message) {
    errno = 0;
    stream_.flush();
    if (!stream_.good()) {
      const auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::PacketStore, DiskWriteOperation::Flush,
          packet_file_path_, StreamErrorCode(), 0, std::move(message));
      std::lock_guard lock(mutex_);
      LatchFaultLocked(fault, true);
      return false;
    }
    std::lock_guard lock(mutex_);
    ++writer_stats_.flush_count;
    return true;
  }

  void Complete(const std::shared_ptr<Completion>& completion,
                bool stream_succeeded) {
    if (completion == nullptr) {
      return;
    }
    DiskWriteFault fault;
    {
      std::lock_guard lock(mutex_);
      fault = last_write_fault_;
    }
    {
      std::lock_guard lock(completion->mutex);
      completion->succeeded = stream_succeeded && !fault.Failed();
      completion->fault = std::move(fault);
      completion->done = true;
    }
    completion->ready.notify_all();
  }

  static bool WaitForCompletion(const std::shared_ptr<Completion>& completion,
                                std::wstring* error) {
    std::unique_lock lock(completion->mutex);
    completion->ready.wait(lock, [&completion] { return completion->done; });
    if (!completion->succeeded) {
      SetError(error, completion->fault.Failed()
                          ? DescribeDiskWriteFault(completion->fault)
                          : L"The packet writer did not complete the request.");
    }
    return completion->succeeded;
  }

  void AddLogicalStats(const PacketMetadata& metadata, size_t payload_size) {
    ++logical_stats_.packet_count;
    logical_stats_.payload_byte_count += payload_size;
    auto found = std::find_if(
        logical_stats_.tracks.begin(), logical_stats_.tracks.end(),
        [&metadata](const PacketStoreTrackStats& track) {
          return track.track_id == metadata.track_id;
        });
    if (found == logical_stats_.tracks.end()) {
      logical_stats_.tracks.push_back(
          PacketStoreTrackStats{metadata.track_id, metadata.codec_id});
      found = std::prev(logical_stats_.tracks.end());
    }
    ++found->packet_count;
    found->payload_byte_count += payload_size;
  }

  void LatchFaultLocked(const DiskWriteFault& fault, bool io_failed) {
    if (!last_write_fault_.Failed() && fault.Failed()) {
      last_write_fault_ = fault;
    }
    accepting_ = false;
    io_failed_ = io_failed_ || io_failed;
  }

  std::filesystem::path packet_file_path_;
  PacketStoreWriterOptions options_;
  mutable std::mutex mutex_;
  std::mutex close_mutex_;
  std::condition_variable ready_;
  std::deque<WorkItem> work_;
  std::ofstream stream_;
  std::thread thread_;
  bool open_ = false;
  bool accepting_ = false;
  bool closing_ = false;
  bool close_when_drained_ = false;
  bool io_failed_ = false;
  std::vector<PacketIndexEntry> persisted_index_;
  PacketStoreStats logical_stats_;
  PacketStoreWriterStats writer_stats_;
  DiskWriteFault last_write_fault_;
};

bool PacketIndexEntry::IsKeyframe() const noexcept {
  return (metadata.flags & PacketFlagKeyframe) != 0;
}

bool PacketStoreRecoveryInfo::HasTruncatedTail() const noexcept {
  return trailing_bytes != 0;
}

int64_t PacketIndexEntry::EndPtsNs() const noexcept {
  if (metadata.duration_ns <= 0) {
    return metadata.pts_ns + 1;
  }

  return metadata.pts_ns + metadata.duration_ns;
}

bool PacketStoreExportSnapshot::IsReady() const noexcept {
  return !session_dir.empty() && !packet_file_path.empty();
}

bool PacketStoreWriterOptions::IsValid() const noexcept {
  return max_queued_packet_count != 0 && max_queued_payload_bytes != 0;
}

PacketRange QueryPacketRange(
    std::span<const PacketIndexEntry> index,
    int64_t start_ns,
    int64_t end_ns,
    bool include_previous_keyframe) {
  PacketRange range;
  range.requested_start_ns = start_ns;
  range.requested_end_ns = end_ns;
  range.actual_start_ns = start_ns;
  range.actual_end_ns = end_ns;

  if (end_ns <= start_ns) {
    return range;
  }

  if (include_previous_keyframe) {
    for (const auto& entry : index) {
      if (entry.metadata.codec_id == CodecId::H264 && entry.IsKeyframe() &&
          entry.metadata.pts_ns <= start_ns) {
        range.actual_start_ns = entry.metadata.pts_ns;
      }
    }
  }

  for (const auto& entry : index) {
    if (include_previous_keyframe &&
        entry.metadata.codec_id == CodecId::H264 &&
        entry.metadata.pts_ns < range.actual_start_ns) {
      continue;
    }
    if (RangesOverlap(entry.metadata.pts_ns, entry.EndPtsNs(),
                      range.actual_start_ns, range.actual_end_ns)) {
      range.packets.push_back(entry);
    }
  }

  return range;
}

PacketStore::~PacketStore() {
  Close();
}

PacketStore::PacketStore(PacketStore&& other) noexcept {
  MoveFrom(std::move(other));
}

PacketStore& PacketStore::operator=(PacketStore&& other) noexcept {
  if (this != &other) {
    Close();
    MoveFrom(std::move(other));
  }
  return *this;
}

PacketStore PacketStore::Create(const std::filesystem::path& session_dir,
                                std::span<const TrackDefinition> tracks,
                                std::wstring* error,
                                DiskWriteFault* write_fault,
                                PacketStoreWriterOptions writer_options) {
  return OpenForCreate(session_dir, tracks, error, write_fault,
                       writer_options);
}

PacketStore PacketStore::Recover(const std::filesystem::path& session_dir,
                                 std::wstring* error,
                                 PacketStoreRecoveryInfo* recovery_info) {
  if (recovery_info != nullptr) {
    *recovery_info = {};
  }
  PacketStore store;
  store.session_dir_ = session_dir;
  store.packet_file_path_ = session_dir / L"packets.dat";

  std::ifstream input(store.packet_file_path_, std::ios::binary);
  if (!input.is_open()) {
    SetError(error, L"Could not open packets.dat for recovery.");
    return {};
  }

  std::error_code file_error;
  const auto file_size =
      std::filesystem::file_size(store.packet_file_path_, file_error);
  if (file_error) {
    SetError(error, L"Could not inspect packets.dat for recovery.");
    return {};
  }
  if (recovery_info != nullptr) {
    recovery_info->file_size = file_size;
  }

  for (;;) {
    const auto file_offset = static_cast<uint64_t>(input.tellg());

    DiskPacketHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    const std::streamsize bytes_read = input.gcount();

    if (bytes_read == 0) {
      break;
    }

    if (bytes_read != sizeof(header)) {
      break;
    }

    if (header.magic != kPacketMagic || header.version != kPacketVersion ||
        header.header_size != sizeof(DiskPacketHeader)) {
      SetError(error, L"packets.dat contains an invalid packet header.");
      return {};
    }

    const uint64_t packet_size = sizeof(DiskPacketHeader) + header.payload_size;
    if (header.payload_size >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        packet_size < header.payload_size) {
      SetError(error, L"packets.dat contains an oversized packet payload.");
      return {};
    }

    if (file_offset > file_size || packet_size > file_size - file_offset) {
      break;
    }

    const auto codec_id = static_cast<CodecId>(header.codec_id);
    if (header.track_id == 0 || !IsKnownCodec(codec_id)) {
      SetError(error, L"packets.dat contains invalid packet metadata.");
      return {};
    }

    input.seekg(static_cast<std::streamoff>(file_offset + packet_size),
                std::ios::beg);

    PacketMetadata metadata;
    metadata.track_id = header.track_id;
    metadata.codec_id = codec_id;
    metadata.flags = header.flags;
    metadata.pts_ns = header.pts_ns;
    metadata.dts_ns = header.dts_ns;
    metadata.duration_ns = header.duration_ns;

    store.tracks_.try_emplace(metadata.track_id, metadata.codec_id);
    store.AddIndexEntry(file_offset, packet_size, header.payload_size, metadata);
    if (recovery_info != nullptr) {
      recovery_info->recovered_bytes = file_offset + packet_size;
      recovery_info->packet_count = store.index_.size();
    }
  }

  if (recovery_info != nullptr) {
    recovery_info->trailing_bytes =
        file_size - std::min(file_size, recovery_info->recovered_bytes);
  }

  return store;
}

bool PacketStore::IsWritable() const noexcept {
  std::shared_ptr<AsyncPacketWriter> writer;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
  }
  return writer != nullptr && writer->IsWritable();
}

bool PacketStore::AppendPacket(const PacketMetadata& metadata,
                               std::span<const std::byte> payload,
                               std::wstring* error) {
  std::shared_ptr<AsyncPacketWriter> writer;
  {
    std::lock_guard lock(mutex_);
    if (!ValidateTrack(metadata, error)) {
      return false;
    }
    writer = writer_;
    if (writer == nullptr) {
      SetError(error, last_write_fault_.Failed()
                          ? DescribeDiskWriteFault(last_write_fault_)
                          : L"PacketStore is not open for writing.");
      return false;
    }
  }

  const bool queued = writer->Enqueue(metadata, payload, error);
  if (!queued) {
    const auto fault = writer->last_write_fault();
    if (fault.Failed()) {
      std::lock_guard lock(mutex_);
      LatchWriteFault(fault);
    }
  }
  return queued;
}

std::vector<PacketIndexEntry> PacketStore::SnapshotIndex() const {
  std::shared_ptr<AsyncPacketWriter> writer;
  std::vector<PacketIndexEntry> recovered_index;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
    recovered_index = index_;
  }
  if (writer == nullptr) {
    return recovered_index;
  }
  std::wstring ignored;
  (void)writer->Flush(&ignored);
  return writer->SnapshotIndex();
}

PacketStoreStats PacketStore::SnapshotStats() const {
  std::shared_ptr<AsyncPacketWriter> writer;
  PacketStoreStats recovered_stats;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
    recovered_stats = stats_;
  }
  return writer == nullptr ? recovered_stats : writer->SnapshotStats();
}

PacketStoreWriterStats PacketStore::SnapshotWriterStats() const {
  std::shared_ptr<AsyncPacketWriter> writer;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
  }
  return writer == nullptr ? PacketStoreWriterStats{}
                           : writer->SnapshotWriterStats();
}

bool PacketStore::SnapshotForExport(PacketStoreExportSnapshot* snapshot,
                                    std::wstring* error) {
  if (snapshot == nullptr) {
    SetError(error, L"PacketStore export snapshot needs a destination.");
    return false;
  }

  *snapshot = {};
  std::shared_ptr<AsyncPacketWriter> writer;
  std::vector<PacketIndexEntry> recovered_index;
  {
    std::lock_guard lock(mutex_);
    if (session_dir_.empty() || packet_file_path_.empty()) {
      SetError(error,
               L"PacketStore export snapshot needs an open or recovered store.");
      return false;
    }
    snapshot->session_dir = session_dir_;
    snapshot->packet_file_path = packet_file_path_;
    writer = writer_;
    recovered_index = index_;
  }

  if (writer == nullptr) {
    snapshot->index = std::move(recovered_index);
    return true;
  }

  if (!writer->Flush(error)) {
    const auto fault = writer->last_write_fault();
    if (fault.Failed()) {
      std::lock_guard lock(mutex_);
      LatchWriteFault(fault);
    }
    return false;
  }
  snapshot->index = writer->SnapshotIndex();
  return true;
}

PacketRange PacketStore::QueryRange(int64_t start_ns, int64_t end_ns,
                                    bool include_previous_keyframe) {
  PacketStoreExportSnapshot snapshot;
  std::wstring error;
  if (!SnapshotForExport(&snapshot, &error)) {
    return QueryPacketRange({}, start_ns, end_ns,
                            include_previous_keyframe);
  }
  return QueryPacketRange(snapshot.index, start_ns, end_ns,
                          include_previous_keyframe);
}

bool PacketStore::ReadPayload(const PacketIndexEntry& entry,
                              std::vector<std::byte>* payload,
                              std::wstring* error) const {
  if (payload == nullptr) {
    SetError(error, L"ReadPayload needs a destination buffer.");
    return false;
  }

  std::ifstream input(packet_file_path_, std::ios::binary);
  if (!input.is_open()) {
    SetError(error, L"Could not open packets.dat for payload read.");
    return false;
  }

  const uint64_t payload_offset =
      entry.file_offset + static_cast<uint64_t>(sizeof(DiskPacketHeader));
  if (payload_offset >
      static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    SetError(error, L"Packet payload offset is too large.");
    return false;
  }

  if (entry.payload_size >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    SetError(error, L"Packet payload is too large to read.");
    return false;
  }

  payload->assign(static_cast<size_t>(entry.payload_size), std::byte{0});
  input.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  if (!payload->empty()) {
    input.read(reinterpret_cast<char*>(payload->data()),
               static_cast<std::streamsize>(payload->size()));
  }

  if (!input.good() && !input.eof()) {
    SetError(error, L"Could not read packet payload.");
    return false;
  }

  return static_cast<uint64_t>(input.gcount()) == entry.payload_size ||
         entry.payload_size == 0;
}

bool PacketStore::Close(std::wstring* error) {
  std::shared_ptr<AsyncPacketWriter> writer;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
  }
  if (writer != nullptr && !writer->Close(error)) {
    const auto fault = writer->last_write_fault();
    if (fault.Failed()) {
      std::lock_guard lock(mutex_);
      LatchWriteFault(fault);
    }
    return false;
  }
  const auto fault = last_write_fault();
  if (fault.Failed()) {
    SetError(error, DescribeDiskWriteFault(fault));
    return false;
  }
  return true;
}

DiskWriteFault PacketStore::last_write_fault() const {
  std::shared_ptr<AsyncPacketWriter> writer;
  DiskWriteFault fault;
  {
    std::lock_guard lock(mutex_);
    writer = writer_;
    fault = last_write_fault_;
  }
  if (!fault.Failed() && writer != nullptr) {
    fault = writer->last_write_fault();
  }
  return fault;
}

const std::filesystem::path& PacketStore::session_dir() const noexcept {
  return session_dir_;
}

const std::filesystem::path& PacketStore::packet_file_path() const noexcept {
  return packet_file_path_;
}

PacketStore PacketStore::OpenForCreate(
    const std::filesystem::path& session_dir,
    std::span<const TrackDefinition> tracks, std::wstring* error,
    DiskWriteFault* write_fault,
    PacketStoreWriterOptions writer_options) {
  if (write_fault != nullptr) {
    *write_fault = {};
  }
  PacketStore store;
  store.session_dir_ = session_dir;
  store.packet_file_path_ = session_dir / L"packets.dat";

  if (!writer_options.IsValid()) {
    SetError(error, L"Packet writer queue limits must be nonzero.");
    return {};
  }

  for (const auto& track : tracks) {
    if (track.track_id == 0 || track.codec_id == CodecId::Unknown) {
      SetError(error, L"Track definitions must include nonzero ids and codecs.");
      return {};
    }

    const auto [_, inserted] =
        store.tracks_.try_emplace(track.track_id, track.codec_id);
    if (!inserted) {
      SetError(error, L"Duplicate PacketStore track id.");
      return {};
    }
    store.stats_.tracks.push_back(
        PacketStoreTrackStats{track.track_id, track.codec_id});
  }

  std::error_code fs_error;
  std::filesystem::create_directories(session_dir, fs_error);
  if (fs_error) {
    const auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::PacketStore,
        DiskWriteOperation::CreateDirectories, session_dir, fs_error);
    if (write_fault != nullptr) {
      *write_fault = fault;
    }
    SetError(error, DescribeDiskWriteFault(fault));
    return {};
  }
  try {
    store.writer_ = std::make_shared<AsyncPacketWriter>(
        store.packet_file_path_, tracks, writer_options);
  } catch (const std::exception&) {
    const auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::PacketStore, DiskWriteOperation::OpenFile,
        store.packet_file_path_, std::make_error_code(std::errc::not_enough_memory),
        0, L"Could not allocate the asynchronous packet writer.");
    if (write_fault != nullptr) {
      *write_fault = fault;
    }
    SetError(error, DescribeDiskWriteFault(fault));
    return {};
  }
  if (!store.writer_->Start(error, write_fault)) {
    return {};
  }

  return store;
}

bool PacketStore::ValidateTrack(const PacketMetadata& metadata,
                                std::wstring* error) const {
  if (metadata.track_id == 0 || !IsKnownCodec(metadata.codec_id)) {
    SetError(error, L"Packet metadata must include a nonzero track and codec.");
    return false;
  }

  const auto found = tracks_.find(metadata.track_id);
  if (found == tracks_.end()) {
    SetError(error, L"Packet references an unknown track id.");
    return false;
  }

  if (found->second != metadata.codec_id) {
    SetError(error, L"Packet codec does not match the track definition.");
    return false;
  }

  if (metadata.duration_ns < 0) {
    SetError(error, L"Packet duration must not be negative.");
    return false;
  }

  return true;
}

void PacketStore::AddIndexEntry(uint64_t file_offset, uint64_t packet_size,
                                uint64_t payload_size,
                                const PacketMetadata& metadata) {
  PacketIndexEntry entry;
  entry.file_offset = file_offset;
  entry.packet_size = packet_size;
  entry.payload_size = payload_size;
  entry.metadata = metadata;
  index_.push_back(entry);
  ++stats_.packet_count;
  stats_.payload_byte_count += payload_size;
  auto found = std::find_if(
      stats_.tracks.begin(), stats_.tracks.end(),
      [&metadata](const PacketStoreTrackStats& track) {
        return track.track_id == metadata.track_id;
      });
  if (found == stats_.tracks.end()) {
    stats_.tracks.push_back(
        PacketStoreTrackStats{metadata.track_id, metadata.codec_id});
    found = std::prev(stats_.tracks.end());
  }
  ++found->packet_count;
  found->payload_byte_count += payload_size;
}

void PacketStore::LatchWriteFault(DiskWriteFault fault) {
  if (!last_write_fault_.Failed() && fault.Failed()) {
    last_write_fault_ = std::move(fault);
  }
}

void PacketStore::MoveFrom(PacketStore&& other) noexcept {
  std::lock_guard lock(other.mutex_);
  session_dir_ = std::move(other.session_dir_);
  packet_file_path_ = std::move(other.packet_file_path_);
  writer_ = std::move(other.writer_);
  tracks_ = std::move(other.tracks_);
  index_ = std::move(other.index_);
  stats_ = std::move(other.stats_);
  last_write_fault_ = std::move(other.last_write_fault_);
}

}  // namespace olouie::record
