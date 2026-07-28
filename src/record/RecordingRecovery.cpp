#include "record/RecordingRecovery.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#include "product/ProductIdentity.h"

#include "record/DiskWriteFault.h"
#include "record/Mp4Muxer.h"
#include "record/PacketStore.h"
#include "record/SessionManifest.h"
#include "record/VideoExportPlan.h"

namespace olouie::record {
namespace {

constexpr std::wstring_view kSessionPrefix = L"recording-";

RecordingRecoveryCommandResult CommandResult(
    RecordingRecoveryCommandStatus status, std::wstring message = {}) {
  return {status, std::move(message)};
}

RecordingRecoveryActionResult ActionResult(
    RecordingRecoveryActionStatus status, std::wstring message = {}) {
  RecordingRecoveryActionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

void RetainFirstError(RecordingRecoveryScanResult* result,
                      const std::wstring& error) {
  if (result != nullptr && result->first_error.empty() && !error.empty()) {
    result->first_error = error;
  }
}

uint32_t ReadBe32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

uint64_t ReadBe64(const unsigned char* bytes) {
  return (static_cast<uint64_t>(ReadBe32(bytes)) << 32) |
         ReadBe32(bytes + 4);
}

bool IsCompleteMp4(const std::filesystem::path& path) {
  std::error_code error;
  const uint64_t file_size = std::filesystem::file_size(path, error);
  if (error || file_size < 24) {
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  bool has_ftyp = false;
  bool has_moov = false;
  bool has_mdat = false;
  uint64_t offset = 0;
  std::array<unsigned char, 16> header{};
  while (offset < file_size) {
    if (file_size - offset < 8) {
      return false;
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    input.read(reinterpret_cast<char*>(header.data()), 8);
    if (input.gcount() != 8) {
      return false;
    }

    uint64_t box_size = ReadBe32(header.data());
    uint64_t header_size = 8;
    if (box_size == 1) {
      if (file_size - offset < 16) {
        return false;
      }
      input.read(reinterpret_cast<char*>(header.data() + 8), 8);
      if (input.gcount() != 8) {
        return false;
      }
      box_size = ReadBe64(header.data() + 8);
      header_size = 16;
    } else if (box_size == 0) {
      box_size = file_size - offset;
    }
    if (box_size < header_size || box_size > file_size - offset) {
      return false;
    }

    const std::array<unsigned char, 4> type{
        header[4], header[5], header[6], header[7]};
    const uint64_t payload_size = box_size - header_size;
    if (offset == 0 &&
        type != std::array<unsigned char, 4>{'f', 't', 'y', 'p'}) {
      return false;
    }
    has_ftyp = has_ftyp ||
               (type == std::array<unsigned char, 4>{'f', 't', 'y', 'p'} &&
                payload_size >= 8);
    has_moov = has_moov ||
               (type == std::array<unsigned char, 4>{'m', 'o', 'o', 'v'} &&
                payload_size != 0);
    has_mdat = has_mdat ||
               (type == std::array<unsigned char, 4>{'m', 'd', 'a', 't'} &&
                payload_size != 0);
    offset += box_size;
  }
  return offset == file_size && has_ftyp && has_moov && has_mdat;
}

bool SessionStem(const std::filesystem::path& session_directory,
                 std::wstring* stem) {
  const std::wstring name = session_directory.filename().wstring();
  if (!name.starts_with(kSessionPrefix) || name.size() == kSessionPrefix.size()) {
    return false;
  }
  if (stem != nullptr) {
    *stem = name.substr(kSessionPrefix.size());
  }
  return true;
}

int64_t RecordingEndNs(const PacketStore& store, uint32_t video_track_id) {
  int64_t end_ns = 0;
  for (const auto& packet : store.SnapshotIndex()) {
    if (packet.metadata.track_id == video_track_id &&
        packet.metadata.codec_id == CodecId::H264) {
      end_ns = std::max(end_ns, packet.EndPtsNs());
    }
  }
  return end_ns;
}

VideoExportPlanResult BuildRecoveryPlan(
    const std::filesystem::path& session_directory, VideoExportPlan* plan) {
  SessionManifest manifest;
  const auto manifest_read = ReadSessionManifest(session_directory, &manifest);
  if (!manifest_read.Succeeded()) {
    VideoExportPlanResult result;
    result.status = VideoExportPlanStatus::ManifestReadFailed;
    result.message = manifest_read.message;
    return result;
  }

  std::wstring recover_error;
  auto store = PacketStore::Recover(session_directory, &recover_error);
  if (!recover_error.empty() || store.session_dir().empty()) {
    VideoExportPlanResult result;
    result.status = VideoExportPlanStatus::PacketStoreRecoverFailed;
    result.message = recover_error;
    return result;
  }

  const int64_t end_ns = RecordingEndNs(store, manifest.video.track_id);
  if (end_ns <= 0) {
    VideoExportPlanResult result;
    result.status = VideoExportPlanStatus::NoPackets;
    result.message = L"The recovered session has no exportable H.264 packets.";
    return result;
  }

  VideoExportPlanOptions options;
  options.requested_start_ns = 0;
  options.requested_end_ns = end_ns;
  return BuildVideoExportPlan(manifest, &store, options, plan);
}

RecordingRecoveryCandidate InspectSession(
    const std::filesystem::path& session_directory,
    const std::filesystem::path& output_directory) {
  RecordingRecoveryCandidate candidate;
  candidate.session_directory = session_directory;
  candidate.manifest_path = SessionManifestPath(session_directory);
  candidate.manifest_temp_path = candidate.manifest_path;
  candidate.manifest_temp_path += L".tmp";
  candidate.packet_path = session_directory / L"packets.dat";
  candidate.can_discard = true;

  std::wstring stem;
  if (!SessionStem(session_directory, &stem)) {
    candidate.kind = RecordingRecoveryKind::Corrupt;
    candidate.message = L"The session directory name is not recognized.";
    return candidate;
  }
  candidate.final_output_path = output_directory /
                                (product::kRecordingFilePrefix + stem +
                                 L".mp4");
  candidate.temp_output_path = candidate.final_output_path;
  candidate.temp_output_path.replace_extension(L".partial.mp4");

  std::error_code error;
  if (std::filesystem::exists(candidate.final_output_path, error) && !error) {
    candidate.kind = RecordingRecoveryKind::AlreadyExported;
    candidate.can_discard = false;
    candidate.message = L"The final MP4 already exists.";
    return candidate;
  }
  error.clear();
  candidate.has_complete_temporary_mp4 =
      std::filesystem::exists(candidate.temp_output_path, error) && !error &&
      IsCompleteMp4(candidate.temp_output_path);

  SessionManifest manifest;
  auto manifest_read = ReadSessionManifest(session_directory, &manifest);
  if (!manifest_read.Succeeded()) {
    SessionManifest temporary_manifest;
    const auto temporary_read =
        ReadSessionManifestFile(candidate.manifest_temp_path,
                                &temporary_manifest);
    if (temporary_read.Succeeded()) {
      manifest = std::move(temporary_manifest);
      manifest_read = temporary_read;
      candidate.uses_temporary_manifest = true;
    }
  }

  if (!manifest_read.Succeeded()) {
    candidate.kind = RecordingRecoveryKind::IncompleteMetadata;
    candidate.can_export = candidate.has_complete_temporary_mp4;
    candidate.message = candidate.can_export
                            ? L"Metadata is incomplete, but a complete unpublished MP4 can be recovered."
                            : L"The session manifest is missing or incomplete.";
    return candidate;
  }
  candidate.manifest_version = manifest.version;

  std::wstring recover_error;
  PacketStoreRecoveryInfo recovery_info;
  auto store = PacketStore::Recover(session_directory, &recover_error,
                                    &recovery_info);
  candidate.packet_count = recovery_info.packet_count;
  candidate.recovered_packet_bytes = recovery_info.recovered_bytes;
  candidate.trailing_packet_bytes = recovery_info.trailing_bytes;
  if (!recover_error.empty() || store.session_dir().empty()) {
    candidate.kind = RecordingRecoveryKind::Corrupt;
    candidate.can_export = candidate.has_complete_temporary_mp4;
    candidate.message = candidate.can_export
                            ? L"Packet data is corrupt, but a complete unpublished MP4 can be recovered."
                            : (recover_error.empty()
                                   ? L"The packet store could not be recovered."
                                   : std::move(recover_error));
    return candidate;
  }

  const int64_t end_ns = RecordingEndNs(store, manifest.video.track_id);
  VideoExportPlan plan;
  VideoExportPlanOptions export_options;
  export_options.requested_start_ns = 0;
  export_options.requested_end_ns = end_ns;
  const auto planned = BuildVideoExportPlan(
      manifest, &store, export_options, &plan);
  if (!planned.Succeeded()) {
    candidate.kind = RecordingRecoveryKind::Corrupt;
    candidate.can_export = candidate.has_complete_temporary_mp4;
    candidate.message = candidate.can_export
                            ? L"The durable session cannot be remuxed, but a complete unpublished MP4 can be recovered."
                            : planned.message;
    return candidate;
  }

  candidate.can_export = true;
  if (candidate.uses_temporary_manifest) {
    candidate.kind = RecordingRecoveryKind::IncompleteMetadata;
    candidate.message = L"A complete unpublished manifest and durable packets are recoverable.";
  } else if (recovery_info.HasTruncatedTail()) {
    candidate.kind = RecordingRecoveryKind::RecoverablePrefix;
    candidate.message = L"A complete packet prefix is recoverable; the truncated tail will be ignored.";
  } else {
    candidate.kind = RecordingRecoveryKind::Complete;
    candidate.message = candidate.has_complete_temporary_mp4
                            ? L"A complete unpublished MP4 is ready to publish."
                            : L"The durable session is ready to recover.";
  }
  return candidate;
}

RecordingRecoveryScanResult DefaultScanRunner(
    const RecordingRecoveryScanOptions& options) {
  return ScanRecordingSessions(options);
}

}  // namespace

size_t RecordingRecoveryScanResult::ExportableCount() const noexcept {
  return static_cast<size_t>(std::count_if(
      candidates.begin(), candidates.end(),
      [](const auto& candidate) { return candidate.can_export; }));
}

size_t RecordingRecoveryScanResult::DiscardableCount() const noexcept {
  return static_cast<size_t>(std::count_if(
      candidates.begin(), candidates.end(),
      [](const auto& candidate) { return candidate.can_discard; }));
}

RecordingRecoveryScanResult ScanRecordingSessions(
    const RecordingRecoveryScanOptions& options) {
  RecordingRecoveryScanResult result;
  if (options.session_root_directory.empty() || options.output_directory.empty() ||
      options.maximum_session_directories == 0) {
    result.first_error = L"Recovery scan paths and session limit are required.";
    return result;
  }

  std::error_code error;
  if (!std::filesystem::exists(options.session_root_directory, error)) {
    if (error) {
      result.first_error = L"Could not inspect the recording session root: " +
                           options.session_root_directory.wstring();
      return result;
    }
    result.succeeded = true;
    return result;
  }

  std::vector<std::filesystem::path> session_directories;
  std::filesystem::directory_iterator iterator(
      options.session_root_directory,
      std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto& entry = *iterator;
    std::error_code entry_error;
    if (entry.is_directory(entry_error) && !entry_error &&
        SessionStem(entry.path(), nullptr)) {
      session_directories.push_back(entry.path());
    }
    iterator.increment(error);
  }
  if (error) {
    result.first_error = L"Recovery scan could not enumerate the session root.";
    return result;
  }

  std::sort(session_directories.begin(), session_directories.end());
  result.discovered_session_count = session_directories.size();
  if (session_directories.size() > options.maximum_session_directories) {
    session_directories.resize(options.maximum_session_directories);
    result.truncated = true;
  }

  for (const auto& session_directory : session_directories) {
    auto candidate = InspectSession(session_directory,
                                    options.output_directory);
    if (candidate.kind == RecordingRecoveryKind::Corrupt ||
        (candidate.kind == RecordingRecoveryKind::IncompleteMetadata &&
         !candidate.can_export)) {
      RetainFirstError(&result,
                       session_directory.filename().wstring() + L": " +
                           candidate.message);
    }
    result.candidates.push_back(std::move(candidate));
  }
  result.scanned_session_count = result.candidates.size();
  result.succeeded = true;
  return result;
}

bool RecordingRecoveryActionResult::Succeeded() const noexcept {
  return status == RecordingRecoveryActionStatus::Success;
}

RecordingRecoveryActionResult ExportRecoveredRecording(
    const RecordingRecoveryCandidate& candidate) {
  if (!candidate.can_export || candidate.session_directory.empty() ||
      candidate.final_output_path.empty() ||
      candidate.temp_output_path.empty()) {
    return ActionResult(RecordingRecoveryActionStatus::InvalidRequest,
                        L"The selected recording is not exportable.");
  }

  std::error_code error;
  if (std::filesystem::exists(candidate.final_output_path, error) && !error) {
    return ActionResult(RecordingRecoveryActionStatus::DestinationExists,
                        L"The recovered recording destination already exists.");
  }
  if (error) {
    return ActionResult(RecordingRecoveryActionStatus::FileSystemError,
                        L"Could not inspect the recovery output path.");
  }

  if (candidate.has_complete_temporary_mp4 &&
      IsCompleteMp4(candidate.temp_output_path)) {
    auto published = Mp4Muxer::AtomicRename(candidate.temp_output_path,
                                            candidate.final_output_path, false);
    if (!published.Succeeded()) {
      return ActionResult(
          published.status == Mp4MuxStatus::DestinationExists
              ? RecordingRecoveryActionStatus::DestinationExists
              : RecordingRecoveryActionStatus::FileSystemError,
          published.message);
    }
    auto result = ActionResult(RecordingRecoveryActionStatus::Success,
                               L"Published the complete recovered MP4.");
    result.output_path = candidate.final_output_path;
    result.retained_session_path = candidate.session_directory;
    return result;
  }

  if (candidate.uses_temporary_manifest) {
    DiskWriteFault fault;
    if (!AtomicPublishFile(candidate.manifest_temp_path,
                           candidate.manifest_path, false,
                           DiskWriteSubsystem::SessionManifest, &fault)) {
      return ActionResult(
          fault.kind == DiskWriteFaultKind::DestinationExists
              ? RecordingRecoveryActionStatus::DestinationExists
              : RecordingRecoveryActionStatus::ManifestPublishFailed,
          DescribeDiskWriteFault(fault));
    }
  }

  VideoExportPlan plan;
  const auto planned = BuildRecoveryPlan(candidate.session_directory, &plan);
  if (!planned.Succeeded()) {
    return ActionResult(RecordingRecoveryActionStatus::PlanFailed,
                        planned.message);
  }

  Mp4MuxRequest request;
  const auto built = BuildVideoMp4MuxRequest(
      plan, candidate.temp_output_path, candidate.final_output_path, false,
      &request);
  if (!built.Succeeded()) {
    return ActionResult(RecordingRecoveryActionStatus::PlanFailed,
                        built.message);
  }
  const auto written = Mp4Muxer{}.WriteMp4(request);
  if (!written.Succeeded()) {
    return ActionResult(
        written.status == Mp4MuxStatus::DestinationExists
            ? RecordingRecoveryActionStatus::DestinationExists
            : RecordingRecoveryActionStatus::MuxFailed,
        written.message);
  }

  auto result = ActionResult(RecordingRecoveryActionStatus::Success,
                             L"Recovered the interrupted recording.");
  result.output_path = candidate.final_output_path;
  result.retained_session_path = candidate.session_directory;
  return result;
}

RecordingRecoveryActionResult DiscardRecoveredRecording(
    const RecordingRecoveryCandidate& candidate) {
  if (!candidate.can_discard || candidate.session_directory.empty()) {
    return ActionResult(RecordingRecoveryActionStatus::InvalidRequest,
                        L"The selected recording cannot be discarded.");
  }

  const auto discarded_root =
      candidate.session_directory.parent_path() / L"discarded";
  const auto retained_path =
      discarded_root / candidate.session_directory.filename();
  std::error_code error;
  std::filesystem::create_directories(discarded_root, error);
  if (error) {
    return ActionResult(RecordingRecoveryActionStatus::FileSystemError,
                        L"Could not create the discarded-session directory.");
  }
  if (std::filesystem::exists(retained_path, error) || error) {
    return ActionResult(
        error ? RecordingRecoveryActionStatus::FileSystemError
              : RecordingRecoveryActionStatus::DestinationExists,
        error ? L"Could not inspect the discarded-session destination."
              : L"A discarded session with this name already exists.");
  }
  std::filesystem::rename(candidate.session_directory, retained_path, error);
  if (error) {
    return ActionResult(RecordingRecoveryActionStatus::FileSystemError,
                        L"Could not move the interrupted session to discarded storage.");
  }

  auto result = ActionResult(
      RecordingRecoveryActionStatus::Success,
      L"Moved the interrupted session to reversible discarded storage.");
  result.retained_session_path = retained_path;
  return result;
}

bool RecordingRecoveryCommandResult::Accepted() const noexcept {
  return status == RecordingRecoveryCommandStatus::Accepted;
}

RecordingRecoverySession::RecordingRecoverySession(
    RecordingRecoveryScanOptions options,
    RecordingRecoveryScanRunner scan_runner,
    RecordingRecoveryActionRunner export_runner,
    RecordingRecoveryActionRunner discard_runner)
    : options_(std::move(options)),
      scan_runner_(scan_runner ? std::move(scan_runner) : &DefaultScanRunner),
      export_runner_(export_runner ? std::move(export_runner)
                                   : &ExportRecoveredRecording),
      discard_runner_(discard_runner ? std::move(discard_runner)
                                     : &DiscardRecoveredRecording) {}

RecordingRecoverySession::~RecordingRecoverySession() {
  Shutdown();
}

void RecordingRecoverySession::SetStateSink(
    RecordingRecoveryStateSink sink) {
  std::lock_guard lock(mutex_);
  state_sink_ = std::move(sink);
}

RecordingRecoveryCommandResult RecordingRecoverySession::StartScan() {
  std::thread completed_worker;
  RecordingRecoverySnapshot scanning;
  RecordingRecoveryStateSink sink;
  uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || snapshot_.state == RecordingRecoveryState::Scanning ||
        snapshot_.state == RecordingRecoveryState::Exporting ||
        snapshot_.state == RecordingRecoveryState::Discarding) {
      return CommandResult(RecordingRecoveryCommandStatus::InvalidState,
                           L"Recording recovery is already busy.");
    }
    if (worker_.joinable()) {
      completed_worker = std::move(worker_);
    }
    generation = ++snapshot_.generation;
    snapshot_.state = RecordingRecoveryState::Scanning;
    snapshot_.message = L"Scanning interrupted recording sessions.";
    scanning = snapshot_;
    sink = state_sink_;
  }
  if (completed_worker.joinable()) {
    completed_worker.join();
  }
  if (sink) {
    sink(scanning);
  }
  StartWorker(RecordingRecoveryActionKind::None, {}, generation);
  return CommandResult(RecordingRecoveryCommandStatus::Accepted);
}

RecordingRecoveryCommandResult RecordingRecoverySession::ExportFirst() {
  return StartAction(RecordingRecoveryActionKind::Export);
}

RecordingRecoveryCommandResult RecordingRecoverySession::DiscardFirst() {
  return StartAction(RecordingRecoveryActionKind::Discard);
}

RecordingRecoveryCommandResult RecordingRecoverySession::StartAction(
    RecordingRecoveryActionKind kind) {
  std::thread completed_worker;
  RecordingRecoveryCandidate selected;
  RecordingRecoverySnapshot working;
  RecordingRecoveryStateSink sink;
  uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || snapshot_.state != RecordingRecoveryState::Ready) {
      return CommandResult(RecordingRecoveryCommandStatus::InvalidState,
                           L"Recording recovery is not ready for an action.");
    }
    const auto found = std::find_if(
        snapshot_.scan.candidates.begin(), snapshot_.scan.candidates.end(),
        [kind](const auto& candidate) {
          return kind == RecordingRecoveryActionKind::Export
                     ? candidate.can_export
                     : candidate.can_discard;
        });
    if (found == snapshot_.scan.candidates.end()) {
      return CommandResult(RecordingRecoveryCommandStatus::NoCandidate,
                           kind == RecordingRecoveryActionKind::Export
                               ? L"No interrupted recording can be recovered."
                               : L"No interrupted recording can be discarded.");
    }
    selected = *found;
    if (worker_.joinable()) {
      completed_worker = std::move(worker_);
    }
    generation = ++snapshot_.generation;
    snapshot_.state = kind == RecordingRecoveryActionKind::Export
                          ? RecordingRecoveryState::Exporting
                          : RecordingRecoveryState::Discarding;
    snapshot_.message = kind == RecordingRecoveryActionKind::Export
                            ? L"Recovering interrupted recording."
                            : L"Moving interrupted recording to discarded storage.";
    working = snapshot_;
    sink = state_sink_;
  }
  if (completed_worker.joinable()) {
    completed_worker.join();
  }
  if (sink) {
    sink(working);
  }
  StartWorker(kind, std::move(selected), generation);
  return CommandResult(RecordingRecoveryCommandStatus::Accepted);
}

void RecordingRecoverySession::StartWorker(
    RecordingRecoveryActionKind kind,
    RecordingRecoveryCandidate candidate,
    uint64_t generation) {
  try {
    worker_ = std::thread([this, kind, candidate = std::move(candidate),
                           generation]() mutable {
      WorkerMain(kind, std::move(candidate), generation);
    });
  } catch (...) {
    RecordingRecoverySnapshot failed;
    {
      std::lock_guard lock(mutex_);
      snapshot_.state = RecordingRecoveryState::Failed;
      snapshot_.message = L"Could not start the recording recovery worker.";
      failed = snapshot_;
    }
    Publish(std::move(failed));
  }
}

void RecordingRecoverySession::WorkerMain(
    RecordingRecoveryActionKind kind,
    RecordingRecoveryCandidate candidate,
    uint64_t generation) {
  RecordingRecoveryActionResult action;
  if (kind == RecordingRecoveryActionKind::Export) {
    action = export_runner_(candidate);
  } else if (kind == RecordingRecoveryActionKind::Discard) {
    action = discard_runner_(candidate);
  }

  RecordingRecoveryScanResult scan;
  if (kind == RecordingRecoveryActionKind::None || action.Succeeded()) {
    scan = scan_runner_(options_);
  }

  RecordingRecoverySnapshot updated;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || snapshot_.generation != generation) {
      return;
    }
    if (kind != RecordingRecoveryActionKind::None) {
      ++snapshot_.action_generation;
      snapshot_.action_kind = kind;
      snapshot_.action = action;
    }
    if (kind != RecordingRecoveryActionKind::None && !action.Succeeded()) {
      snapshot_.state = RecordingRecoveryState::Ready;
      snapshot_.message = action.message;
    } else {
      snapshot_.scan = std::move(scan);
      snapshot_.state = snapshot_.scan.succeeded
                            ? RecordingRecoveryState::Ready
                            : RecordingRecoveryState::Failed;
      snapshot_.message = snapshot_.scan.succeeded
                              ? (kind == RecordingRecoveryActionKind::None
                                     ? L"Recording recovery scan completed."
                                     : action.message)
                              : snapshot_.scan.first_error;
    }
    updated = snapshot_;
  }
  Publish(std::move(updated));
}

void RecordingRecoverySession::Publish(RecordingRecoverySnapshot snapshot) {
  RecordingRecoveryStateSink sink;
  {
    std::lock_guard lock(mutex_);
    sink = state_sink_;
  }
  if (sink) {
    sink(snapshot);
  }
}

RecordingRecoverySnapshot RecordingRecoverySession::Snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void RecordingRecoverySession::Shutdown() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) {
      return;
    }
    shutting_down_ = true;
    state_sink_ = {};
    if (worker_.joinable()) {
      worker = std::move(worker_);
    }
  }
  if (worker.joinable()) {
    worker.join();
  }
}

const wchar_t* RecordingRecoveryKindName(
    RecordingRecoveryKind kind) noexcept {
  switch (kind) {
    case RecordingRecoveryKind::Complete:
      return L"complete";
    case RecordingRecoveryKind::RecoverablePrefix:
      return L"recoverable prefix";
    case RecordingRecoveryKind::IncompleteMetadata:
      return L"incomplete metadata";
    case RecordingRecoveryKind::Corrupt:
      return L"corrupt";
    case RecordingRecoveryKind::AlreadyExported:
      return L"already exported";
  }
  return L"unknown";
}

const wchar_t* RecordingRecoveryStateName(
    RecordingRecoveryState state) noexcept {
  switch (state) {
    case RecordingRecoveryState::Idle:
      return L"idle";
    case RecordingRecoveryState::Scanning:
      return L"scanning";
    case RecordingRecoveryState::Ready:
      return L"ready";
    case RecordingRecoveryState::Exporting:
      return L"exporting";
    case RecordingRecoveryState::Discarding:
      return L"discarding";
    case RecordingRecoveryState::Failed:
      return L"failed";
  }
  return L"unknown";
}

}  // namespace olouie::record
