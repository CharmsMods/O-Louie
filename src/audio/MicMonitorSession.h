#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "audio/PcmAudio.h"

namespace olouie::audio {

enum class MicMonitorState {
  Idle,
  Starting,
  Monitoring,
  Stopping,
  Failed,
};

struct MicMonitorOptions {
  std::wstring output_device_id;
};

enum class MicMonitorCommandStatus {
  Accepted,
  AlreadyRunning,
  NotRunning,
  ShuttingDown,
  ThreadError,
};

struct MicMonitorCommandResult {
  MicMonitorCommandStatus status = MicMonitorCommandStatus::ThreadError;
  std::wstring message;

  bool Accepted() const noexcept;
};

struct MicMonitorSnapshot {
  uint64_t generation = 0;
  MicMonitorState state = MicMonitorState::Idle;
  std::wstring requested_output_device_id;
  std::wstring active_output_device_id;
  std::wstring active_output_device_name;
  bool using_fallback_output = false;
  float peak_dbfs = -60.0f;
  bool clipping = false;
  uint64_t underrun_count = 0;
  uint64_t overflow_count = 0;
  uint32_t queued_frame_count = 0;
  uint32_t queue_capacity_frames = 0;
  std::wstring message;
};

struct MicMonitorBackendUpdate {
  bool monitoring_started = false;
  std::wstring active_output_device_id;
  std::wstring active_output_device_name;
  bool using_fallback_output = false;
  float peak_dbfs = -60.0f;
  bool clipping = false;
  uint64_t underrun_count = 0;
  uint64_t overflow_count = 0;
  uint32_t queued_frame_count = 0;
  uint32_t queue_capacity_frames = 0;
  std::wstring message;
};

enum class MicMonitorBackendStatus {
  Stopped,
  Failed,
};

struct MicMonitorBackendResult {
  MicMonitorBackendStatus status = MicMonitorBackendStatus::Failed;
  std::wstring message;
};

class IMicMonitorBackend {
 public:
  using UpdateSink = std::function<void(const MicMonitorBackendUpdate&)>;

  virtual ~IMicMonitorBackend() = default;
  virtual MicMonitorBackendResult Run(const MicMonitorOptions& options,
                                      const std::atomic_bool& stop_requested,
                                      UpdateSink update_sink) = 0;
};

using MicMonitorBackendFactory =
    std::function<std::unique_ptr<IMicMonitorBackend>()>;

namespace detail {

class MicMonitorPcmFifo final {
 public:
  MicMonitorPcmFifo(uint32_t capacity_frames, uint16_t block_align);

  uint32_t capacity_frames() const noexcept;
  uint32_t size_frames() const noexcept;
  bool Push(const std::byte* source, uint32_t frame_count, bool silent);
  uint32_t Pop(std::byte* destination, uint32_t requested_frames);

 private:
  void WriteFrames(uint32_t destination_frame, uint32_t frame_count,
                   const std::byte* source, bool silent);
  void CopyFrames(std::byte* destination, uint32_t source_frame,
                  uint32_t frame_count) const;

  uint32_t capacity_frames_ = 0;
  uint16_t block_align_ = 0;
  std::vector<std::byte> bytes_;
  uint32_t read_frame_ = 0;
  uint32_t size_frames_ = 0;
};

}  // namespace detail

class MicMonitorSession final {
 public:
  using StateSink = std::function<void(const MicMonitorSnapshot&)>;

  explicit MicMonitorSession(MicMonitorBackendFactory backend_factory = {});
  ~MicMonitorSession();

  MicMonitorSession(const MicMonitorSession&) = delete;
  MicMonitorSession& operator=(const MicMonitorSession&) = delete;

  void SetStateSink(StateSink sink);
  MicMonitorCommandResult Start(MicMonitorOptions options);
  MicMonitorCommandResult Stop();
  void Shutdown();
  MicMonitorSnapshot Snapshot() const;

 private:
  void WorkerMain(MicMonitorOptions options);
  void ApplyBackendUpdate(const MicMonitorBackendUpdate& update);
  void Publish(MicMonitorSnapshot snapshot, const StateSink& sink) const;
  void ReapCompletedWorker();

  MicMonitorBackendFactory backend_factory_;
  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic_bool stop_requested_ = false;
  MicMonitorSnapshot snapshot_;
  StateSink state_sink_;
  bool worker_running_ = false;
  bool shutting_down_ = false;
};

float MeasurePcmPeakDbfs(const PcmStreamFormat& format,
                         std::span<const std::byte> pcm_bytes,
                         bool silent) noexcept;

const wchar_t* MicMonitorStateName(MicMonitorState state) noexcept;
const wchar_t* MicMonitorCommandStatusName(
    MicMonitorCommandStatus status) noexcept;

}  // namespace olouie::audio
