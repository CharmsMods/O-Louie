#include "record/VideoRecorderSession.h"

#include <utility>

namespace olouie::record {
namespace {

VideoRecorderPipelineResult DefaultPipelineRunner(
    const VideoRecorderPipelineOptions& options,
    std::atomic_bool* stop_requested,
    VideoRecorderPipelineStageSink stage_sink) {
  return RunVideoRecorderPipeline(options, stop_requested,
                                  std::move(stage_sink));
}

VideoRecorderCommandResult CommandResult(VideoRecorderCommandStatus status,
                                         std::wstring message) {
  return {status, std::move(message)};
}

bool IsActive(VideoRecorderState state) noexcept {
  return state == VideoRecorderState::Starting ||
         state == VideoRecorderState::Recording ||
         state == VideoRecorderState::Stopping;
}

}  // namespace

bool VideoRecorderCommandResult::Accepted() const noexcept {
  return status == VideoRecorderCommandStatus::Accepted;
}

VideoRecorderSession::VideoRecorderSession(
    VideoRecorderPipelineOptions options,
    VideoRecorderPipelineRunner runner)
    : options_(std::move(options)),
      runner_(runner ? std::move(runner) : &DefaultPipelineRunner),
      clip_commands_(std::make_shared<VideoRecorderClipCommandQueue>()) {
  options_.clip_commands = clip_commands_;
  options_.clip_event_sink =
      [this](const VideoRecorderClipEvent& event) { HandleClipEvent(event); };
}

VideoRecorderSession::~VideoRecorderSession() {
  Shutdown();
}

void VideoRecorderSession::SetStateSink(VideoRecorderStateSink sink) {
  std::lock_guard lock(mutex_);
  state_sink_ = std::move(sink);
}

VideoRecorderCommandResult VideoRecorderSession::Start() {
  std::thread completed_worker;
  uint64_t generation = 0;
  VideoRecorderSnapshot starting_snapshot;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) {
      return CommandResult(VideoRecorderCommandStatus::InvalidState,
                           L"Recorder session is shutting down.");
    }
    if (IsActive(snapshot_.state)) {
      return CommandResult(VideoRecorderCommandStatus::InvalidState,
                           L"A recording operation is already active.");
    }
    if (!runner_) {
      return CommandResult(VideoRecorderCommandStatus::InvalidConfig,
                           L"Recorder pipeline runner is unavailable.");
    }
    if (worker_.joinable()) {
      completed_worker = std::move(worker_);
    }
    stop_requested_.store(false);
    generation = ++snapshot_.generation;
    snapshot_.state = VideoRecorderState::Starting;
    snapshot_.message = L"Preparing recording sources and encoders.";
    snapshot_.session_directory.clear();
    snapshot_.output_path.clear();
    snapshot_.stats = {};
    ++snapshot_.diagnostics_generation;
    snapshot_.recording_saved_after_failure = false;
    snapshot_.clip = {};
    starting_snapshot = snapshot_;
    sink = state_sink_;
  }

  if (completed_worker.joinable()) {
    completed_worker.join();
  }
  if (sink) {
    sink(starting_snapshot);
  }

  try {
    worker_ = std::thread([this, generation] { WorkerMain(generation); });
  } catch (...) {
    PublishState(generation, VideoRecorderState::Failed,
                 L"Could not start the recorder worker thread.");
    return CommandResult(VideoRecorderCommandStatus::InvalidConfig,
                         L"Could not start the recorder worker thread.");
  }
  return CommandResult(VideoRecorderCommandStatus::Accepted, L"");
}

VideoRecorderCommandResult VideoRecorderSession::StopAndSave() {
  uint64_t generation = 0;
  VideoRecorderSnapshot stopping_snapshot;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.state != VideoRecorderState::Starting &&
        snapshot_.state != VideoRecorderState::Recording) {
      return CommandResult(VideoRecorderCommandStatus::InvalidState,
                           L"No active recording can be stopped and saved.");
    }
    stop_requested_.store(true);
    generation = snapshot_.generation;
    snapshot_.state = VideoRecorderState::Stopping;
    snapshot_.message = L"Stopping and saving the recording.";
    stopping_snapshot = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(stopping_snapshot);
  }
  return CommandResult(VideoRecorderCommandStatus::Accepted, L"");
}

VideoRecorderCommandResult VideoRecorderSession::SaveLastClip(
    std::chrono::milliseconds duration) {
  VideoRecorderSnapshot queued_snapshot;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ ||
        snapshot_.state != VideoRecorderState::Recording) {
      return CommandResult(VideoRecorderCommandStatus::InvalidState,
                           L"Clips can only be saved while recording.");
    }
    if (clip_commands_ == nullptr) {
      return CommandResult(VideoRecorderCommandStatus::InvalidConfig,
                           L"Clip command queue is unavailable.");
    }

    const auto queued = clip_commands_->Enqueue(duration);
    if (!queued.Accepted()) {
      return CommandResult(
          queued.status == VideoRecorderClipCommandStatus::QueueFull
              ? VideoRecorderCommandStatus::QueueFull
              : VideoRecorderCommandStatus::InvalidConfig,
          queued.message);
    }

    ++snapshot_.clip_event_generation;
    snapshot_.clip = {};
    snapshot_.clip.kind = VideoRecorderExportKind::Clip;
    snapshot_.clip.state = VideoRecorderClipState::Queued;
    snapshot_.clip.request_id = queued.request_id;
    snapshot_.clip.duration = duration;
    snapshot_.clip.output_path.clear();
    snapshot_.clip.message = L"Clip queued.";
    queued_snapshot = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(queued_snapshot);
  }
  return CommandResult(VideoRecorderCommandStatus::Accepted, L"");
}

VideoRecorderCommandResult VideoRecorderSession::AddBookmarkAndSave(
    std::chrono::milliseconds pre_roll,
    std::chrono::milliseconds post_roll) {
  VideoRecorderSnapshot queued_snapshot;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ ||
        snapshot_.state != VideoRecorderState::Recording) {
      return CommandResult(VideoRecorderCommandStatus::InvalidState,
                           L"Bookmarks can only be added while recording.");
    }
    if (clip_commands_ == nullptr) {
      return CommandResult(VideoRecorderCommandStatus::InvalidConfig,
                           L"Bookmark command queue is unavailable.");
    }

    const auto queued =
        clip_commands_->EnqueueBookmark(pre_roll, post_roll);
    if (!queued.Accepted()) {
      return CommandResult(
          queued.status == VideoRecorderClipCommandStatus::QueueFull
              ? VideoRecorderCommandStatus::QueueFull
              : VideoRecorderCommandStatus::InvalidConfig,
          queued.message);
    }

    ++snapshot_.clip_event_generation;
    snapshot_.clip = {};
    snapshot_.clip.kind = VideoRecorderExportKind::Bookmark;
    snapshot_.clip.state = VideoRecorderClipState::Queued;
    snapshot_.clip.request_id = queued.request_id;
    snapshot_.clip.duration = pre_roll + post_roll;
    snapshot_.clip.message = L"Bookmark request queued.";
    queued_snapshot = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(queued_snapshot);
  }
  return CommandResult(VideoRecorderCommandStatus::Accepted, L"");
}

void VideoRecorderSession::Shutdown() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ && !worker_.joinable()) {
      return;
    }
    shutting_down_ = true;
    stop_requested_.store(true);
    if (clip_commands_ != nullptr) {
      clip_commands_->Shutdown();
    }
    if (worker_.joinable()) {
      worker = std::move(worker_);
    }
  }
  if (worker.joinable()) {
    worker.join();
  }
  std::lock_guard lock(mutex_);
  snapshot_.state = VideoRecorderState::Idle;
  snapshot_.message.clear();
}

VideoRecorderSnapshot VideoRecorderSession::Snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void VideoRecorderSession::WorkerMain(uint64_t generation) {
  VideoRecorderPipelineResult result;
  try {
    auto run_options = options_;
    const auto external_progress_sink = run_options.progress_sink;
    run_options.progress_sink =
        [this, generation, external_progress_sink](
            const VideoRecorderPipelineProgress& progress) {
          if (external_progress_sink) {
            external_progress_sink(progress);
          }
          HandlePipelineProgress(generation, progress);
        };
    result = runner_(
        run_options, &stop_requested_,
        [this, generation](VideoRecorderPipelineStage stage) {
          HandlePipelineStage(generation, stage);
        });
  } catch (...) {
    result.status = VideoRecorderPipelineStatus::PrepareFailed;
    result.failed_stage = VideoRecorderPipelineStage::Prepare;
    result.message = L"Recorder pipeline failed unexpectedly.";
  }

  if (result.Succeeded()) {
    PublishState(generation, VideoRecorderState::Saved,
                 L"Recording saved.", &result);
    return;
  }
  if (result.status == VideoRecorderPipelineStatus::Cancelled) {
    PublishState(generation, VideoRecorderState::Idle,
                 result.message.empty() ? L"Recording start was cancelled."
                                        : result.message,
                 &result);
    return;
  }

  std::wstring message = result.recording_saved_after_failure
                             ? L"Recording stopped"
                             : L"Recording failed";
  if (result.failed_stage != VideoRecorderPipelineStage::Complete) {
    message += L" during ";
    message += VideoRecorderPipelineStageName(result.failed_stage);
  }
  if (!result.message.empty()) {
    message += L": ";
    message += result.message;
  }
  PublishState(generation, VideoRecorderState::Failed, std::move(message),
               &result);
}

void VideoRecorderSession::HandlePipelineProgress(
    uint64_t generation,
    const VideoRecorderPipelineProgress& progress) {
  VideoRecorderSnapshot published;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.generation != generation || shutting_down_ ||
        !IsActive(snapshot_.state)) {
      return;
    }
    snapshot_.session_directory = progress.session_directory;
    snapshot_.output_path = progress.output_path;
    snapshot_.stats = progress.stats;
    ++snapshot_.diagnostics_generation;
    published = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(published);
  }
}

void VideoRecorderSession::HandlePipelineStage(
    uint64_t generation,
    VideoRecorderPipelineStage stage) {
  if (stage == VideoRecorderPipelineStage::Recording) {
    PublishState(generation, VideoRecorderState::Recording,
                 L"Recording is active.");
  } else if (stage == VideoRecorderPipelineStage::CaptureStop) {
    PublishState(generation, VideoRecorderState::Stopping,
                 L"Stopping capture and saving the full recording.");
  }
}

void VideoRecorderSession::HandleClipEvent(
    const VideoRecorderClipEvent& event) {
  VideoRecorderSnapshot published;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || event.request_id == 0 ||
        event.state == VideoRecorderClipState::None) {
      return;
    }
    ++snapshot_.clip_event_generation;
    snapshot_.clip = event;
    published = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(published);
  }
}

void VideoRecorderSession::PublishState(
    uint64_t generation,
    VideoRecorderState state,
    std::wstring message,
    const VideoRecorderPipelineResult* result) {
  VideoRecorderSnapshot published;
  VideoRecorderStateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.generation != generation || shutting_down_) {
      return;
    }
    if (state == VideoRecorderState::Recording &&
        snapshot_.state == VideoRecorderState::Stopping) {
      return;
    }
    snapshot_.state = state;
    snapshot_.message = std::move(message);
    if (result != nullptr) {
      snapshot_.session_directory = result->session_directory;
      snapshot_.output_path = result->output_path;
      snapshot_.stats = result->stats;
      snapshot_.recording_saved_after_failure =
          result->recording_saved_after_failure;
    }
    ++snapshot_.diagnostics_generation;
    published = snapshot_;
    sink = state_sink_;
  }
  if (sink) {
    sink(published);
  }
}

const wchar_t* VideoRecorderStateName(VideoRecorderState state) noexcept {
  switch (state) {
    case VideoRecorderState::Idle:
      return L"idle";
    case VideoRecorderState::Starting:
      return L"starting";
    case VideoRecorderState::Recording:
      return L"recording";
    case VideoRecorderState::Stopping:
      return L"stopping";
    case VideoRecorderState::Saved:
      return L"saved";
    case VideoRecorderState::Failed:
      return L"failed";
  }
  return L"unknown";
}

const wchar_t* VideoRecorderCommandStatusName(
    VideoRecorderCommandStatus status) noexcept {
  switch (status) {
    case VideoRecorderCommandStatus::Accepted:
      return L"accepted";
    case VideoRecorderCommandStatus::InvalidState:
      return L"invalid state";
    case VideoRecorderCommandStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecorderCommandStatus::QueueFull:
      return L"queue full";
  }
  return L"unknown";
}

}  // namespace olouie::record
