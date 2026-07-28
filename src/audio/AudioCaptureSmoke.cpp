#include "audio/AudioCaptureSmoke.h"

#include <chrono>
#include <string>
#include <utility>

#include "audio/WasapiLoopbackCapture.h"
#include "audio/WasapiMicCapture.h"

namespace olouie::audio {
namespace {

AudioCaptureSmokeRunResult Result(AudioCaptureSmokeStatus status,
                                  std::wstring message) {
  AudioCaptureSmokeRunResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

AudioCaptureSmokeStatus StatusFromManager(
    AudioCaptureManagerStatus status) noexcept {
  switch (status) {
    case AudioCaptureManagerStatus::Success:
      return AudioCaptureSmokeStatus::Success;
    case AudioCaptureManagerStatus::InvalidPlan:
    case AudioCaptureManagerStatus::MissingSink:
    case AudioCaptureManagerStatus::UnsupportedPlan:
    case AudioCaptureManagerStatus::DuplicateSource:
      return AudioCaptureSmokeStatus::InvalidConfig;
  }

  return AudioCaptureSmokeStatus::InvalidConfig;
}

AudioCaptureSourceSmokeResult SourceResultFromBinding(
    const AudioCaptureSourceBinding& binding) {
  AudioCaptureSourceSmokeResult result;
  result.source = binding.source;
  result.runtime = binding.runtime;
  result.support = binding.support;
  result.track_id = binding.track_id;
  return result;
}

AudioCaptureSmokeSourceRunner RunnerForRuntime(
    AudioCaptureSourceRuntime runtime,
    const AudioCaptureSmokeRunners& runners) noexcept {
  switch (runtime) {
    case AudioCaptureSourceRuntime::SystemLoopback:
      return runners.system_loopback;
    case AudioCaptureSourceRuntime::Microphone:
      return runners.microphone;
    case AudioCaptureSourceRuntime::ProcessLoopback:
      return nullptr;
  }

  return nullptr;
}

std::wstring MissingRunnerMessage(AudioCaptureSourceRuntime runtime) {
  return std::wstring(L"Audio capture smoke has no runner for ") +
         AudioCaptureSourceRuntimeName(runtime) + L".";
}

}  // namespace

bool AudioCaptureSmokeRunResult::Succeeded() const noexcept {
  return status == AudioCaptureSmokeStatus::Success;
}

AudioCaptureSmokeRunners DefaultAudioCaptureSmokeRunners() noexcept {
  AudioCaptureSmokeRunners runners;
  runners.system_loopback =
      static_cast<AudioCaptureSmokeSourceRunner>(
          &RunDefaultRenderLoopbackSmoke);
  runners.microphone =
      static_cast<AudioCaptureSmokeSourceRunner>(&RunDefaultMicCaptureSmoke);
  return runners;
}

AudioCaptureSmokeRunResult RunAudioCaptureSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    ICapturedPcmSink* sink,
    AudioCaptureSmokeResult* result) {
  return RunAudioCaptureSmoke(plan, duration, sink,
                              DefaultAudioCaptureSmokeRunners(), result);
}

AudioCaptureSmokeRunResult RunAudioCaptureSmoke(
    const AudioTrackPlan& plan,
    std::chrono::milliseconds duration,
    ICapturedPcmSink* sink,
    const AudioCaptureSmokeRunners& runners,
    AudioCaptureSmokeResult* result) {
  if (result == nullptr) {
    return Result(AudioCaptureSmokeStatus::InvalidConfig,
                  L"Audio capture smoke needs an output destination.");
  }

  *result = {};

  if (duration <= std::chrono::milliseconds(0) ||
      duration > std::chrono::seconds(30)) {
    return Result(AudioCaptureSmokeStatus::InvalidConfig,
                  L"Audio capture smoke duration must be between 1 ms and "
                  L"30 s.");
  }

  AudioCaptureManager manager;
  const auto configured = manager.Configure(plan, sink);
  if (!configured.Succeeded()) {
    return Result(StatusFromManager(configured.status), configured.message);
  }

  result->deferred_mixed_track = manager.has_deferred_mixed_track();

  bool source_failed = false;
  std::wstring first_failure;

  for (const auto& binding : manager.sources()) {
    AudioCaptureSourceSmokeResult source_result =
        SourceResultFromBinding(binding);

    if (binding.support == AudioCaptureSourceSupport::Deferred) {
      source_result.message =
          std::wstring(AudioCaptureSourceRuntimeName(binding.runtime)) +
          L" capture is deferred.";
      ++result->deferred_source_count;
      result->sources.push_back(std::move(source_result));
      continue;
    }

    auto* runner = RunnerForRuntime(binding.runtime, runners);
    if (runner == nullptr) {
      source_result.attempted = true;
      source_result.message = MissingRunnerMessage(binding.runtime);
      ++result->attempted_source_count;
      result->sources.push_back(source_result);
      if (first_failure.empty()) {
        first_failure = source_result.message;
      }
      return Result(AudioCaptureSmokeStatus::InvalidConfig, first_failure);
    }

    source_result.attempted = true;
    ++result->attempted_source_count;

    std::wstring source_error;
    source_result.succeeded =
        runner(duration, &source_result.capture, binding.sink, &source_error);
    if (source_result.succeeded) {
      ++result->succeeded_source_count;
      result->packet_count += source_result.capture.packet_count;
      result->frame_count += source_result.capture.frame_count;
    } else {
      source_failed = true;
      source_result.message = std::move(source_error);
      if (first_failure.empty()) {
        first_failure =
            std::wstring(AudioCaptureSourceRuntimeName(binding.runtime)) +
            L" smoke failed: " + source_result.message;
      }
    }

    result->sources.push_back(std::move(source_result));
  }

  if (source_failed) {
    return Result(AudioCaptureSmokeStatus::SourceFailed,
                  std::move(first_failure));
  }

  return Result(AudioCaptureSmokeStatus::Success, L"");
}

}  // namespace olouie::audio
