#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "record/VideoRecorderPipeline.h"

namespace olouie::record {

enum class VideoRecorderState {
  Idle,
  Starting,
  Recording,
  Stopping,
  Saved,
  Failed,
};

enum class VideoRecorderCommandStatus {
  Accepted,
  InvalidState,
  InvalidConfig,
  QueueFull,
};

struct VideoRecorderCommandResult {
  VideoRecorderCommandStatus status =
      VideoRecorderCommandStatus::InvalidConfig;
  std::wstring message;

  bool Accepted() const noexcept;
};

struct VideoRecorderSnapshot {
  uint64_t generation = 0;
  VideoRecorderState state = VideoRecorderState::Idle;
  std::wstring message;
  std::filesystem::path session_directory;
  std::filesystem::path output_path;
  VideoRecorderPipelineStats stats;
  uint64_t diagnostics_generation = 0;
  bool recording_saved_after_failure = false;
  uint64_t clip_event_generation = 0;
  VideoRecorderClipEvent clip;
};

using VideoRecorderStateSink =
    std::function<void(const VideoRecorderSnapshot&)>;
using VideoRecorderPipelineRunner = std::function<VideoRecorderPipelineResult(
    const VideoRecorderPipelineOptions&,
    std::atomic_bool*,
    VideoRecorderPipelineStageSink)>;

class VideoRecorderSession final {
 public:
  explicit VideoRecorderSession(
      VideoRecorderPipelineOptions options,
      VideoRecorderPipelineRunner runner = {});
  ~VideoRecorderSession();

  VideoRecorderSession(const VideoRecorderSession&) = delete;
  VideoRecorderSession& operator=(const VideoRecorderSession&) = delete;

  void SetStateSink(VideoRecorderStateSink sink);
  VideoRecorderCommandResult Start();
  VideoRecorderCommandResult StopAndSave();
  VideoRecorderCommandResult SaveLastClip(
      std::chrono::milliseconds duration);
  VideoRecorderCommandResult AddBookmarkAndSave(
      std::chrono::milliseconds pre_roll,
      std::chrono::milliseconds post_roll);
  void Shutdown();

  VideoRecorderSnapshot Snapshot() const;

 private:
  void WorkerMain(uint64_t generation);
  void HandlePipelineStage(uint64_t generation,
                           VideoRecorderPipelineStage stage);
  void HandlePipelineProgress(
      uint64_t generation,
      const VideoRecorderPipelineProgress& progress);
  void HandleClipEvent(const VideoRecorderClipEvent& event);
  void PublishState(uint64_t generation,
                    VideoRecorderState state,
                    std::wstring message,
                    const VideoRecorderPipelineResult* result = nullptr);

  VideoRecorderPipelineOptions options_;
  VideoRecorderPipelineRunner runner_;
  mutable std::mutex mutex_;
  VideoRecorderSnapshot snapshot_;
  VideoRecorderStateSink state_sink_;
  std::atomic_bool stop_requested_{false};
  std::thread worker_;
  std::shared_ptr<VideoRecorderClipCommandQueue> clip_commands_;
  bool shutting_down_ = false;
};

const wchar_t* VideoRecorderStateName(VideoRecorderState state) noexcept;
const wchar_t* VideoRecorderCommandStatusName(
    VideoRecorderCommandStatus status) noexcept;

}  // namespace olouie::record
