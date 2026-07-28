#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace olouie::record {

enum class VideoRecorderExportKind {
  Clip,
  Bookmark,
};

enum class VideoRecorderClipState {
  None,
  Queued,
  Saved,
  Failed,
};

struct VideoRecorderClipRequest {
  VideoRecorderExportKind kind = VideoRecorderExportKind::Clip;
  uint64_t request_id = 0;
  std::chrono::milliseconds duration{0};
  std::chrono::milliseconds bookmark_pre_roll{0};
  std::chrono::milliseconds bookmark_post_roll{0};
  uint64_t bookmark_id = 0;
  int64_t bookmark_time_ns = 0;
};

enum class VideoRecorderClipCommandStatus {
  Accepted,
  InvalidDuration,
  QueueFull,
  ShuttingDown,
};

struct VideoRecorderClipCommandResult {
  VideoRecorderClipCommandStatus status =
      VideoRecorderClipCommandStatus::InvalidDuration;
  uint64_t request_id = 0;
  std::wstring message;

  bool Accepted() const noexcept;
};

struct VideoRecorderClipEvent {
  VideoRecorderExportKind kind = VideoRecorderExportKind::Clip;
  VideoRecorderClipState state = VideoRecorderClipState::None;
  uint64_t request_id = 0;
  std::chrono::milliseconds duration{0};
  uint64_t bookmark_id = 0;
  int64_t bookmark_time_ns = 0;
  std::filesystem::path output_path;
  std::wstring message;
};

using VideoRecorderClipEventSink =
    std::function<void(const VideoRecorderClipEvent&)>;

class VideoRecorderClipCommandQueue final {
 public:
  explicit VideoRecorderClipCommandQueue(size_t capacity = 4);

  VideoRecorderClipCommandResult Enqueue(
      std::chrono::milliseconds duration);
  VideoRecorderClipCommandResult EnqueueBookmark(
      std::chrono::milliseconds pre_roll,
      std::chrono::milliseconds post_roll);
  bool TryPop(VideoRecorderClipRequest* request);
  void Shutdown();

  size_t pending_count() const;
  size_t capacity() const noexcept;

 private:
  VideoRecorderClipCommandResult EnqueueRequest(
      VideoRecorderClipRequest request);

  const size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<VideoRecorderClipRequest> pending_;
  uint64_t next_request_id_ = 1;
  bool shutting_down_ = false;
};

const wchar_t* VideoRecorderClipStateName(
    VideoRecorderClipState state) noexcept;
const wchar_t* VideoRecorderExportKindName(
    VideoRecorderExportKind kind) noexcept;
const wchar_t* VideoRecorderClipCommandStatusName(
    VideoRecorderClipCommandStatus status) noexcept;

}  // namespace olouie::record
