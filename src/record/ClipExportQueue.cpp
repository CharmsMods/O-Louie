#include "record/ClipExportQueue.h"

#include <windows.h>

#include <exception>
#include <utility>

namespace olouie::record {
namespace {

Mp4MuxResult DefaultWrite(const Mp4MuxRequest& request) {
  return Mp4Muxer{}.WriteMp4(request);
}

ClipExportQueueResult Result(ClipExportQueueStatus status,
                             std::wstring message) {
  return {status, std::move(message)};
}

Mp4MuxResult UnexpectedWriteFailure() {
  return {Mp4MuxStatus::FileSystemError,
          L"Clip MP4 writer failed unexpectedly."};
}

}  // namespace

bool ClipExportQueueResult::Succeeded() const noexcept {
  return status == ClipExportQueueStatus::Success;
}

ClipExportQueue::ClipExportQueue(ClipExportQueueOptions options)
    : options_(std::move(options)) {
  if (!options_.writer) {
    options_.writer = &DefaultWrite;
  }
}

ClipExportQueue::~ClipExportQueue() {
  Shutdown();
}

ClipExportQueueResult ClipExportQueue::Start(
    ClipExportCompletionSink completion_sink) {
  std::lock_guard lock(mutex_);
  if (started_ || options_.capacity == 0 || !options_.writer ||
      !completion_sink) {
    return Result(ClipExportQueueStatus::InvalidConfig,
                  L"Clip export queue configuration is invalid.");
  }

  completion_sink_ = std::move(completion_sink);
  stopping_ = false;
  try {
    worker_ = std::thread([this] { WorkerMain(); });
  } catch (...) {
    completion_sink_ = {};
    return Result(ClipExportQueueStatus::ThreadStartFailed,
                  L"Could not start the clip export worker thread.");
  }
  started_ = true;
  stats_.running = true;
  return Result(ClipExportQueueStatus::Success, L"");
}

ClipExportQueueResult ClipExportQueue::Enqueue(ClipExportJob job) {
  if (job.request_id == 0 ||
      job.duration <= std::chrono::milliseconds(0) ||
      job.output_path.empty() ||
      !Mp4Muxer::ValidateRequest(job.mux_request).Succeeded()) {
    return Result(ClipExportQueueStatus::InvalidConfig,
                  L"Clip export job is incomplete.");
  }

  {
    std::lock_guard lock(mutex_);
    if (!started_) {
      return Result(ClipExportQueueStatus::NotRunning,
                    L"Clip export queue is not running.");
    }
    if (stopping_) {
      return Result(ClipExportQueueStatus::ShuttingDown,
                    L"Clip export queue is shutting down.");
    }
    if (stats_.outstanding_job_count >= options_.capacity) {
      ++stats_.rejected_job_count;
      return Result(ClipExportQueueStatus::QueueFull,
                    L"Clip export queue is full.");
    }

    jobs_.push_back(std::move(job));
    ++stats_.submitted_job_count;
    ++stats_.outstanding_job_count;
  }
  ready_.notify_one();
  return Result(ClipExportQueueStatus::Success, L"");
}

void ClipExportQueue::Shutdown() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (!started_) {
      return;
    }
    stopping_ = true;
    if (worker_.joinable()) {
      worker = std::move(worker_);
    }
  }
  ready_.notify_all();
  if (worker.joinable()) {
    worker.join();
  }

  std::lock_guard lock(mutex_);
  started_ = false;
  stats_.running = false;
  completion_sink_ = {};
}

ClipExportQueueStats ClipExportQueue::Snapshot() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void ClipExportQueue::WorkerMain() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

  for (;;) {
    ClipExportJob job;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) {
        if (stopping_) {
          break;
        }
        continue;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    Mp4MuxResult write_result;
    try {
      write_result = options_.writer(job.mux_request);
    } catch (...) {
      write_result = UnexpectedWriteFailure();
    }

    ClipExportCompletionSink sink;
    {
      std::lock_guard lock(mutex_);
      if (write_result.Succeeded()) {
        ++stats_.saved_job_count;
      } else {
        ++stats_.failed_job_count;
      }
      if (stats_.outstanding_job_count > 0) {
        --stats_.outstanding_job_count;
      }
      sink = completion_sink_;
    }

    if (sink) {
      try {
        sink(ClipExportCompletion{std::move(job),
                                  std::move(write_result)});
      } catch (...) {
      }
    }
  }
}

const wchar_t* ClipExportQueueStatusName(
    ClipExportQueueStatus status) noexcept {
  switch (status) {
    case ClipExportQueueStatus::Success:
      return L"success";
    case ClipExportQueueStatus::InvalidConfig:
      return L"invalid config";
    case ClipExportQueueStatus::NotRunning:
      return L"not running";
    case ClipExportQueueStatus::QueueFull:
      return L"queue full";
    case ClipExportQueueStatus::ShuttingDown:
      return L"shutting down";
    case ClipExportQueueStatus::ThreadStartFailed:
      return L"thread start failed";
  }
  return L"unknown";
}

}  // namespace olouie::record
