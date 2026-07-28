#include "audio/AudioRecordingSession.h"

#include <utility>

namespace olouie::audio {
namespace {

AudioRecordingSessionResult Result(AudioRecordingSessionStatus status,
                                   std::wstring message) {
  AudioRecordingSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

AudioRecordingSessionStatus StatusFromPreflight(
    AudioCaptureEncodeSetupStatus status) noexcept {
  switch (status) {
    case AudioCaptureEncodeSetupStatus::Success:
      return AudioRecordingSessionStatus::Success;
    case AudioCaptureEncodeSetupStatus::InvalidConfig:
      return AudioRecordingSessionStatus::InvalidConfig;
    case AudioCaptureEncodeSetupStatus::SourceUnavailable:
    case AudioCaptureEncodeSetupStatus::UnsupportedFormat:
    case AudioCaptureEncodeSetupStatus::TrackPlanFailed:
    case AudioCaptureEncodeSetupStatus::EncoderInitFailed:
    case AudioCaptureEncodeSetupStatus::BindingFailed:
    case AudioCaptureEncodeSetupStatus::SessionCreateFailed:
      return AudioRecordingSessionStatus::PreflightFailed;
  }

  return AudioRecordingSessionStatus::PreflightFailed;
}

AudioRecordingSessionStatus StatusFromSetup(
    AudioCaptureEncodeSetupStatus status) noexcept {
  switch (status) {
    case AudioCaptureEncodeSetupStatus::Success:
      return AudioRecordingSessionStatus::Success;
    case AudioCaptureEncodeSetupStatus::InvalidConfig:
      return AudioRecordingSessionStatus::InvalidConfig;
    case AudioCaptureEncodeSetupStatus::SourceUnavailable:
    case AudioCaptureEncodeSetupStatus::UnsupportedFormat:
    case AudioCaptureEncodeSetupStatus::TrackPlanFailed:
    case AudioCaptureEncodeSetupStatus::EncoderInitFailed:
    case AudioCaptureEncodeSetupStatus::BindingFailed:
    case AudioCaptureEncodeSetupStatus::SessionCreateFailed:
      return AudioRecordingSessionStatus::SetupFailed;
  }

  return AudioRecordingSessionStatus::SetupFailed;
}

AudioRecordingSessionStatus StatusFromRun(
    AudioLiveCaptureEncodeStatus status) noexcept {
  switch (status) {
    case AudioLiveCaptureEncodeStatus::Success:
      return AudioRecordingSessionStatus::Success;
    case AudioLiveCaptureEncodeStatus::InvalidConfig:
      return AudioRecordingSessionStatus::InvalidConfig;
    case AudioLiveCaptureEncodeStatus::SourceStartFailed:
    case AudioLiveCaptureEncodeStatus::CaptureFailed:
    case AudioLiveCaptureEncodeStatus::DrainFailed:
    case AudioLiveCaptureEncodeStatus::FlushFailed:
      return AudioRecordingSessionStatus::RunFailed;
  }

  return AudioRecordingSessionStatus::RunFailed;
}

std::wstring WithStagePrefix(const wchar_t* stage, std::wstring message) {
  if (message.empty()) {
    return stage;
  }

  return std::wstring(stage) + L": " + message;
}

}  // namespace

bool AudioRecordingSessionResult::Succeeded() const noexcept {
  return status == AudioRecordingSessionStatus::Success;
}

AudioRecordingSession::AudioRecordingSession(
    AudioRecordingSessionOptions options)
    : options_(std::move(options)) {}

AudioRecordingSessionResult AudioRecordingSession::Preflight() {
  setup_.Reset();
  packet_store_ = nullptr;
  last_live_result_ = {};

  AudioCaptureEncodeSetupResult setup_result;
  if (options_.format_provider != nullptr) {
    setup_result = BuildAudioCaptureEncodePreflight(
        options_.preflight, options_.format_provider, &preflight_);
  } else {
    setup_result =
        BuildAudioCaptureEncodePreflight(options_.preflight, &preflight_);
  }

  auto result = Result(StatusFromPreflight(setup_result.status),
                       WithStagePrefix(L"Audio preflight failed",
                                       setup_result.message));
  result.setup_result = setup_result;
  if (setup_result.Succeeded()) {
    result.status = AudioRecordingSessionStatus::Success;
    result.message.clear();
  }

  last_result_ = result;
  return last_result_;
}

AudioRecordingSessionResult AudioRecordingSession::Prepare(
    record::PacketStore* packet_store) {
  setup_.Reset();
  packet_store_ = nullptr;
  last_live_result_ = {};

  if (!preflight_.IsUsable()) {
    last_result_ =
        Result(AudioRecordingSessionStatus::InvalidState,
               L"Audio recording session needs a successful preflight before "
               L"prepare.");
    return last_result_;
  }

  AudioCaptureEncodeSetupResult setup_result;
  if (options_.encoder_factory != nullptr) {
    setup_result = BuildAudioCaptureEncodeSessionSetup(
        preflight_, options_.setup, packet_store, options_.encoder_factory,
        &setup_);
  } else {
    setup_result = BuildAudioCaptureEncodeSessionSetup(
        preflight_, options_.setup, packet_store, &setup_);
  }

  auto result = Result(StatusFromSetup(setup_result.status),
                       WithStagePrefix(L"Audio session setup failed",
                                       setup_result.message));
  result.setup_result = setup_result;
  if (setup_result.Succeeded()) {
    result.status = AudioRecordingSessionStatus::Success;
    result.message.clear();
    packet_store_ = packet_store;
  }

  last_result_ = result;
  return last_result_;
}

AudioRecordingSessionResult AudioRecordingSession::Run() {
  return RunForDuration(options_.live.duration);
}

AudioRecordingSessionResult AudioRecordingSession::RunForDuration(
    std::chrono::milliseconds duration) {
  last_live_result_ = {};

  if (!preflight_.IsUsable() || !setup_.IsConfigured() ||
      setup_.session == nullptr) {
    last_result_ =
        Result(AudioRecordingSessionStatus::InvalidState,
               L"Audio recording session needs a prepared encode session "
               L"before run.");
    return last_result_;
  }

  auto live_options = options_.live;
  live_options.duration = duration;

  AudioLiveCaptureEncodeRunResult live_result;
  if (options_.live_source_factory != nullptr) {
    live_result = RunAudioLiveCaptureEncode(
        preflight_.plan, live_options, setup_.session.get(),
        options_.live_source_factory, &last_live_result_);
  } else {
    live_result = RunAudioLiveCaptureEncode(
        preflight_.plan, live_options, setup_.session.get(),
        &last_live_result_);
  }

  auto result = Result(StatusFromRun(live_result.status),
                       WithStagePrefix(L"Audio live capture encode failed",
                                       live_result.message));
  result.live_result = live_result;
  if (live_result.Succeeded()) {
    result.status = AudioRecordingSessionStatus::Success;
    result.message.clear();
  }

  last_result_ = result;
  return last_result_;
}

void AudioRecordingSession::Reset() {
  preflight_.Reset();
  setup_.Reset();
  packet_store_ = nullptr;
  last_live_result_ = {};
  last_result_ = {};
}

bool AudioRecordingSession::IsPreflighted() const noexcept {
  return preflight_.IsUsable();
}

bool AudioRecordingSession::IsPrepared() const noexcept {
  return setup_.IsConfigured();
}

const AudioRecordingSessionOptions& AudioRecordingSession::options()
    const noexcept {
  return options_;
}

const AudioCaptureEncodePreflight& AudioRecordingSession::preflight()
    const noexcept {
  return preflight_;
}

const AudioCaptureEncodeSessionSetup& AudioRecordingSession::setup()
    const noexcept {
  return setup_;
}

AudioEncodeSession* AudioRecordingSession::encode_session() const noexcept {
  return setup_.session.get();
}

record::PacketStore* AudioRecordingSession::packet_store() const noexcept {
  return packet_store_;
}

const AudioLiveCaptureEncodeResult& AudioRecordingSession::last_live_result()
    const noexcept {
  return last_live_result_;
}

const AudioRecordingSessionResult& AudioRecordingSession::last_result()
    const noexcept {
  return last_result_;
}

const wchar_t* AudioRecordingSessionStatusName(
    AudioRecordingSessionStatus status) noexcept {
  switch (status) {
    case AudioRecordingSessionStatus::Success:
      return L"success";
    case AudioRecordingSessionStatus::InvalidConfig:
      return L"invalid config";
    case AudioRecordingSessionStatus::InvalidState:
      return L"invalid state";
    case AudioRecordingSessionStatus::PreflightFailed:
      return L"preflight failed";
    case AudioRecordingSessionStatus::SetupFailed:
      return L"setup failed";
    case AudioRecordingSessionStatus::RunFailed:
      return L"run failed";
  }

  return L"unknown";
}

}  // namespace olouie::audio
