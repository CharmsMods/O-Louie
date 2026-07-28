#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "record/Mp4Muxer.h"
#include "record/VideoRecorderClip.h"

namespace olouie::record {

struct ClipExportJob {
  VideoRecorderExportKind kind = VideoRecorderExportKind::Clip;
  uint64_t request_id = 0;
  std::chrono::milliseconds duration{0};
  uint64_t bookmark_id = 0;
  int64_t bookmark_time_ns = 0;
  std::filesystem::path output_path;
  Mp4MuxRequest mux_request;
};

struct ClipExportCompletion {
  ClipExportJob job;
  Mp4MuxResult result;
};

using ClipExportCompletionSink =
    std::function<void(const ClipExportCompletion&)>;
using ClipMuxWriter =
    std::function<Mp4MuxResult(const Mp4MuxRequest&)>;

enum class ClipExportQueueStatus {
  Success,
  InvalidConfig,
  NotRunning,
  QueueFull,
  ShuttingDown,
  ThreadStartFailed,
};

struct ClipExportQueueResult {
  ClipExportQueueStatus status = ClipExportQueueStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct ClipExportQueueStats {
  uint64_t submitted_job_count = 0;
  uint64_t saved_job_count = 0;
  uint64_t failed_job_count = 0;
  uint64_t rejected_job_count = 0;
  size_t outstanding_job_count = 0;
  bool running = false;
};

struct ClipExportQueueOptions {
  size_t capacity = 2;
  ClipMuxWriter writer;
};

class ClipExportQueue final {
 public:
  explicit ClipExportQueue(ClipExportQueueOptions options = {});
  ~ClipExportQueue();

  ClipExportQueue(const ClipExportQueue&) = delete;
  ClipExportQueue& operator=(const ClipExportQueue&) = delete;

  ClipExportQueueResult Start(ClipExportCompletionSink completion_sink);
  ClipExportQueueResult Enqueue(ClipExportJob job);
  void Shutdown();

  ClipExportQueueStats Snapshot() const;

 private:
  void WorkerMain();

  ClipExportQueueOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<ClipExportJob> jobs_;
  ClipExportCompletionSink completion_sink_;
  ClipExportQueueStats stats_;
  std::thread worker_;
  bool started_ = false;
  bool stopping_ = false;
};

const wchar_t* ClipExportQueueStatusName(
    ClipExportQueueStatus status) noexcept;

}  // namespace olouie::record
