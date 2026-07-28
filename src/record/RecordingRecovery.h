#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace olouie::record {

enum class RecordingRecoveryKind {
  Complete,
  RecoverablePrefix,
  IncompleteMetadata,
  Corrupt,
  AlreadyExported,
};

struct RecordingRecoveryCandidate {
  RecordingRecoveryKind kind = RecordingRecoveryKind::Corrupt;
  std::filesystem::path session_directory;
  std::filesystem::path manifest_path;
  std::filesystem::path manifest_temp_path;
  std::filesystem::path packet_path;
  std::filesystem::path temp_output_path;
  std::filesystem::path final_output_path;
  uint32_t manifest_version = 0;
  uint64_t packet_count = 0;
  uint64_t recovered_packet_bytes = 0;
  uint64_t trailing_packet_bytes = 0;
  bool uses_temporary_manifest = false;
  bool has_complete_temporary_mp4 = false;
  bool can_export = false;
  bool can_discard = false;
  std::wstring message;
};

struct RecordingRecoveryScanOptions {
  std::filesystem::path session_root_directory;
  std::filesystem::path output_directory;
  size_t maximum_session_directories = 256;
};

struct RecordingRecoveryScanResult {
  bool succeeded = false;
  bool truncated = false;
  size_t discovered_session_count = 0;
  size_t scanned_session_count = 0;
  std::vector<RecordingRecoveryCandidate> candidates;
  std::wstring first_error;

  size_t ExportableCount() const noexcept;
  size_t DiscardableCount() const noexcept;
};

RecordingRecoveryScanResult ScanRecordingSessions(
    const RecordingRecoveryScanOptions& options);

enum class RecordingRecoveryActionStatus {
  Success,
  InvalidRequest,
  DestinationExists,
  ManifestPublishFailed,
  PlanFailed,
  MuxFailed,
  FileSystemError,
};

struct RecordingRecoveryActionResult {
  RecordingRecoveryActionStatus status =
      RecordingRecoveryActionStatus::InvalidRequest;
  std::filesystem::path output_path;
  std::filesystem::path retained_session_path;
  std::wstring message;

  bool Succeeded() const noexcept;
};

RecordingRecoveryActionResult ExportRecoveredRecording(
    const RecordingRecoveryCandidate& candidate);
RecordingRecoveryActionResult DiscardRecoveredRecording(
    const RecordingRecoveryCandidate& candidate);

enum class RecordingRecoveryState {
  Idle,
  Scanning,
  Ready,
  Exporting,
  Discarding,
  Failed,
};

enum class RecordingRecoveryCommandStatus {
  Accepted,
  InvalidState,
  NoCandidate,
};

struct RecordingRecoveryCommandResult {
  RecordingRecoveryCommandStatus status =
      RecordingRecoveryCommandStatus::InvalidState;
  std::wstring message;

  bool Accepted() const noexcept;
};

enum class RecordingRecoveryActionKind {
  None,
  Export,
  Discard,
};

struct RecordingRecoverySnapshot {
  uint64_t generation = 0;
  RecordingRecoveryState state = RecordingRecoveryState::Idle;
  RecordingRecoveryScanResult scan;
  uint64_t action_generation = 0;
  RecordingRecoveryActionKind action_kind = RecordingRecoveryActionKind::None;
  RecordingRecoveryActionResult action;
  std::wstring message;
};

using RecordingRecoveryStateSink =
    std::function<void(const RecordingRecoverySnapshot&)>;
using RecordingRecoveryScanRunner = std::function<RecordingRecoveryScanResult(
    const RecordingRecoveryScanOptions&)>;
using RecordingRecoveryActionRunner =
    std::function<RecordingRecoveryActionResult(
        const RecordingRecoveryCandidate&)>;

class RecordingRecoverySession final {
 public:
  explicit RecordingRecoverySession(
      RecordingRecoveryScanOptions options,
      RecordingRecoveryScanRunner scan_runner = {},
      RecordingRecoveryActionRunner export_runner = {},
      RecordingRecoveryActionRunner discard_runner = {});
  ~RecordingRecoverySession();

  RecordingRecoverySession(const RecordingRecoverySession&) = delete;
  RecordingRecoverySession& operator=(const RecordingRecoverySession&) =
      delete;

  void SetStateSink(RecordingRecoveryStateSink sink);
  RecordingRecoveryCommandResult StartScan();
  RecordingRecoveryCommandResult ExportFirst();
  RecordingRecoveryCommandResult DiscardFirst();
  RecordingRecoverySnapshot Snapshot() const;
  void Shutdown();

 private:
  RecordingRecoveryCommandResult StartAction(
      RecordingRecoveryActionKind kind);
  void StartWorker(RecordingRecoveryActionKind kind,
                   RecordingRecoveryCandidate candidate,
                   uint64_t generation);
  void WorkerMain(RecordingRecoveryActionKind kind,
                  RecordingRecoveryCandidate candidate,
                  uint64_t generation);
  void Publish(RecordingRecoverySnapshot snapshot);

  RecordingRecoveryScanOptions options_;
  RecordingRecoveryScanRunner scan_runner_;
  RecordingRecoveryActionRunner export_runner_;
  RecordingRecoveryActionRunner discard_runner_;
  mutable std::mutex mutex_;
  RecordingRecoverySnapshot snapshot_;
  RecordingRecoveryStateSink state_sink_;
  std::thread worker_;
  bool shutting_down_ = false;
};

const wchar_t* RecordingRecoveryKindName(
    RecordingRecoveryKind kind) noexcept;
const wchar_t* RecordingRecoveryStateName(
    RecordingRecoveryState state) noexcept;

}  // namespace olouie::record
