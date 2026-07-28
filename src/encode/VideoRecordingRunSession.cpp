#include "encode/VideoRecordingRunSession.h"

#include <utility>

namespace olouie::encode {
namespace {

VideoRecordingRunSessionResult Result(
    VideoRecordingRunSessionStatus status,
    std::wstring message) {
  VideoRecordingRunSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

VideoRecordingRunSessionStatus StatusFromSetup(
    VideoRecordingSetupStatus status,
    bool preflight_stage) noexcept {
  switch (status) {
    case VideoRecordingSetupStatus::Success:
      return VideoRecordingRunSessionStatus::Success;
    case VideoRecordingSetupStatus::InvalidConfig:
      return VideoRecordingRunSessionStatus::InvalidConfig;
    case VideoRecordingSetupStatus::EncoderConfigInvalid:
      return preflight_stage ? VideoRecordingRunSessionStatus::PreflightFailed
                             : VideoRecordingRunSessionStatus::SetupFailed;
    case VideoRecordingSetupStatus::SessionCreateFailed:
      return VideoRecordingRunSessionStatus::SetupFailed;
  }

  return preflight_stage ? VideoRecordingRunSessionStatus::PreflightFailed
                         : VideoRecordingRunSessionStatus::SetupFailed;
}

VideoRecordingRunSessionStatus StatusFromRun(
    VideoLiveCaptureEncodeStatus status) noexcept {
  switch (status) {
    case VideoLiveCaptureEncodeStatus::Success:
      return VideoRecordingRunSessionStatus::Success;
    case VideoLiveCaptureEncodeStatus::InvalidConfig:
      return VideoRecordingRunSessionStatus::InvalidConfig;
    case VideoLiveCaptureEncodeStatus::CaptureFailed:
    case VideoLiveCaptureEncodeStatus::DrainFailed:
      return VideoRecordingRunSessionStatus::RunFailed;
  }

  return VideoRecordingRunSessionStatus::RunFailed;
}

std::wstring WithStagePrefix(const wchar_t* stage, std::wstring message) {
  if (message.empty()) {
    return stage;
  }

  return std::wstring(stage) + L": " + message;
}

}  // namespace

bool VideoRecordingRunSessionResult::Succeeded() const noexcept {
  return status == VideoRecordingRunSessionStatus::Success;
}

VideoRecordingRunSession::VideoRecordingRunSession(
    VideoRecordingRunSessionOptions options)
    : options_(std::move(options)) {}

VideoRecordingRunSessionResult VideoRecordingRunSession::Preflight() {
  setup_.Reset();
  encoder_session_ = nullptr;
  d3d_device_ = nullptr;
  d3d_context_ = nullptr;
  packet_store_ = nullptr;
  last_live_result_ = {};

  auto setup_result =
      BuildVideoRecordingPreflight(options_.preflight, &preflight_);
  auto result = Result(StatusFromSetup(setup_result.status, true),
                       WithStagePrefix(L"Video preflight failed",
                                       setup_result.message));
  result.setup_result = setup_result;
  if (setup_result.Succeeded()) {
    result.status = VideoRecordingRunSessionStatus::Success;
    result.message.clear();
  }

  last_result_ = result;
  return last_result_;
}

VideoRecordingRunSessionResult VideoRecordingRunSession::Prepare(
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    ID3D11DeviceContext* d3d_context,
    record::PacketStore* packet_store) {
  setup_.Reset();
  encoder_session_ = nullptr;
  d3d_device_ = nullptr;
  d3d_context_ = nullptr;
  packet_store_ = nullptr;
  last_live_result_ = {};

  if (!preflight_.IsUsable()) {
    last_result_ =
        Result(VideoRecordingRunSessionStatus::InvalidState,
               L"Video recording session needs a successful preflight before "
               L"prepare.");
    return last_result_;
  }

  auto setup_result = BuildVideoRecordingSessionSetup(
      preflight_, encoder_session, d3d_device, d3d_context, packet_store,
      &setup_);
  auto result = Result(StatusFromSetup(setup_result.status, false),
                       WithStagePrefix(L"Video session setup failed",
                                       setup_result.message));
  result.setup_result = setup_result;
  if (setup_result.Succeeded()) {
    result.status = VideoRecordingRunSessionStatus::Success;
    result.message.clear();
    encoder_session_ = encoder_session;
    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    packet_store_ = packet_store;
  }

  last_result_ = result;
  return last_result_;
}

VideoRecordingRunSessionResult VideoRecordingRunSession::Run() {
  return RunForDuration(preflight_.live_options.duration);
}

VideoRecordingRunSessionResult VideoRecordingRunSession::RunForDuration(
    std::chrono::milliseconds duration) {
  last_live_result_ = {};

  if (!preflight_.IsUsable() || !setup_.IsConfigured() ||
      setup_.chain == nullptr || d3d_device_ == nullptr ||
      d3d_context_ == nullptr) {
    last_result_ =
        Result(VideoRecordingRunSessionStatus::InvalidState,
               L"Video recording session needs a prepared encode chain before "
               L"run.");
    return last_result_;
  }

  if (options_.monitor == nullptr) {
    last_result_ =
        Result(VideoRecordingRunSessionStatus::InvalidConfig,
               L"Video recording session needs a monitor handle before run.");
    return last_result_;
  }

  auto live_options = preflight_.live_options;
  live_options.duration = duration;

  VideoLiveCaptureEncodeRunResult live_result;
  if (options_.capture_runner != nullptr) {
    live_result = RunWgcVideoLiveCaptureEncode(
        options_.monitor, d3d_device_, d3d_context_, live_options,
        setup_.chain.get(), options_.capture_runner, &last_live_result_);
  } else {
    live_result = RunWgcVideoLiveCaptureEncode(
        options_.monitor, d3d_device_, d3d_context_, live_options,
        setup_.chain.get(), &last_live_result_);
  }

  auto result = Result(StatusFromRun(live_result.status),
                       WithStagePrefix(L"Video live capture encode failed",
                                       live_result.message));
  result.live_result = live_result;
  if (live_result.Succeeded()) {
    result.status = VideoRecordingRunSessionStatus::Success;
    result.message.clear();
  }

  last_result_ = result;
  return last_result_;
}

void VideoRecordingRunSession::Reset() {
  preflight_.Reset();
  setup_.Reset();
  encoder_session_ = nullptr;
  d3d_device_ = nullptr;
  d3d_context_ = nullptr;
  packet_store_ = nullptr;
  last_live_result_ = {};
  last_result_ = {};
}

bool VideoRecordingRunSession::IsPreflighted() const noexcept {
  return preflight_.IsUsable();
}

bool VideoRecordingRunSession::IsPrepared() const noexcept {
  return setup_.IsConfigured();
}

const VideoRecordingRunSessionOptions& VideoRecordingRunSession::options()
    const noexcept {
  return options_;
}

const VideoRecordingPreflight& VideoRecordingRunSession::preflight()
    const noexcept {
  return preflight_;
}

const VideoRecordingSessionSetup& VideoRecordingRunSession::setup()
    const noexcept {
  return setup_;
}

VideoEncodeChain* VideoRecordingRunSession::encode_chain() const noexcept {
  return setup_.chain.get();
}

MfHardwareH264EncoderSession* VideoRecordingRunSession::encoder_session()
    const noexcept {
  return encoder_session_;
}

record::PacketStore* VideoRecordingRunSession::packet_store() const noexcept {
  return packet_store_;
}

const VideoLiveCaptureEncodeResult&
VideoRecordingRunSession::last_live_result() const noexcept {
  return last_live_result_;
}

const VideoRecordingRunSessionResult& VideoRecordingRunSession::last_result()
    const noexcept {
  return last_result_;
}

const wchar_t* VideoRecordingRunSessionStatusName(
    VideoRecordingRunSessionStatus status) noexcept {
  switch (status) {
    case VideoRecordingRunSessionStatus::Success:
      return L"success";
    case VideoRecordingRunSessionStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecordingRunSessionStatus::InvalidState:
      return L"invalid state";
    case VideoRecordingRunSessionStatus::PreflightFailed:
      return L"preflight failed";
    case VideoRecordingRunSessionStatus::SetupFailed:
      return L"setup failed";
    case VideoRecordingRunSessionStatus::RunFailed:
      return L"run failed";
  }

  return L"unknown";
}

}  // namespace olouie::encode
