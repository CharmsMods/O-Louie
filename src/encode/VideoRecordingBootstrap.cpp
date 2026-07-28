#include "encode/VideoRecordingBootstrap.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace olouie::encode {
namespace {

VideoRecordingBootstrapResult Result(VideoRecordingBootstrapStatus status,
                                     std::wstring message) {
  VideoRecordingBootstrapResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

const graphics::MonitorInfo* ResolveMonitor(
    const std::vector<graphics::MonitorInfo>& monitors,
    HMONITOR requested_monitor) noexcept {
  if (requested_monitor == nullptr) {
    return graphics::FindPrimaryMonitor(monitors);
  }

  for (const auto& monitor : monitors) {
    if (monitor.handle == requested_monitor) {
      return &monitor;
    }
  }

  return nullptr;
}

std::wstring WithStagePrefix(const wchar_t* stage, std::wstring message) {
  if (message.empty()) {
    return stage;
  }

  return std::wstring(stage) + L": " + message;
}

}  // namespace

bool VideoRecordingBootstrapResult::Succeeded() const noexcept {
  return status == VideoRecordingBootstrapStatus::Success;
}

void VideoRecordingBootstrapSession::Reset() {
  recording_session.reset();
  packet_store = record::PacketStore{};
  encoder_session.reset();
  d3d = graphics::D3D11DeviceContext{};
  monitor = {};
}

bool VideoRecordingBootstrapSession::IsPrepared() const noexcept {
  return monitor.handle != nullptr && d3d.IsValid() &&
         encoder_session != nullptr && encoder_session->IsConfigured() &&
         packet_store.IsWritable() && recording_session != nullptr &&
         recording_session->IsPrepared();
}

VideoRecordingBootstrapResult BuildVideoRecordingBootstrapSession(
    const VideoRecordingBootstrapOptions& options,
    VideoRecordingBootstrapSession* session) {
  if (session == nullptr) {
    return Result(VideoRecordingBootstrapStatus::InvalidConfig,
                  L"Video recording bootstrap needs an output destination.");
  }

  session->Reset();

  if (options.packet_store_session_dir.empty()) {
    return Result(VideoRecordingBootstrapStatus::InvalidConfig,
                  L"Video recording bootstrap needs a PacketStore session "
                  L"directory.");
  }

  auto encoder_config_result =
      ValidateMfHardwareH264EncoderConfig(options.preflight.encoder_config);
  if (!encoder_config_result.Succeeded()) {
    auto result =
        Result(VideoRecordingBootstrapStatus::RecordingPreflightFailed,
               WithStagePrefix(L"Video encoder config validation failed",
                               encoder_config_result.message));
    result.encoder_config_result = std::move(encoder_config_result);
    return result;
  }

  const auto monitors = graphics::EnumerateMonitors();
  const auto* monitor = ResolveMonitor(monitors, options.monitor);
  if (monitor == nullptr || monitor->handle == nullptr ||
      monitor->Width() <= 0 || monitor->Height() <= 0) {
    return Result(VideoRecordingBootstrapStatus::MonitorUnavailable,
                  options.monitor == nullptr
                      ? L"Primary monitor is unavailable."
                      : L"Requested monitor is unavailable.");
  }
  session->monitor = *monitor;

  std::wstring d3d_error;
  session->d3d =
      graphics::D3D11DeviceContext::CreateForMonitor(monitor->handle,
                                                     &d3d_error);
  if (!session->d3d.IsValid()) {
    return Result(VideoRecordingBootstrapStatus::D3DCreateFailed,
                  WithStagePrefix(L"D3D11 device creation failed",
                                  std::move(d3d_error)));
  }

  auto run_options = VideoRecordingRunSessionOptions{};
  run_options.monitor = monitor->handle;
  run_options.preflight = options.preflight;
  run_options.preflight.source_width =
      static_cast<uint32_t>(monitor->Width());
  run_options.preflight.source_height =
      static_cast<uint32_t>(monitor->Height());
  run_options.capture_runner = options.capture_runner;

  session->recording_session =
      std::make_unique<VideoRecordingRunSession>(run_options);
  auto preflight_result = session->recording_session->Preflight();
  if (!preflight_result.Succeeded()) {
    auto result =
        Result(VideoRecordingBootstrapStatus::RecordingPreflightFailed,
               preflight_result.message);
    result.encoder_config_result = std::move(encoder_config_result);
    result.preflight_result = std::move(preflight_result);
    return result;
  }

  session->encoder_session =
      std::make_unique<MfHardwareH264EncoderSession>();
  auto encoder_result = session->encoder_session->Initialize(
      run_options.preflight.encoder_config, options.encoder_probe_options,
      session->d3d.device());
  if (!encoder_result.Succeeded()) {
    auto result = Result(VideoRecordingBootstrapStatus::EncoderInitFailed,
                         WithStagePrefix(L"Video encoder initialization failed",
                                         encoder_result.message));
    result.encoder_config_result = std::move(encoder_config_result);
    result.encoder_result = std::move(encoder_result);
    result.preflight_result = std::move(preflight_result);
    return result;
  }

  std::vector<record::TrackDefinition> tracks;
  tracks.reserve(1 + options.additional_packet_tracks.size());
  tracks.push_back(session->recording_session->preflight().video_track);
  tracks.insert(tracks.end(), options.additional_packet_tracks.begin(),
                options.additional_packet_tracks.end());
  std::wstring packet_store_error;
  session->packet_store = record::PacketStore::Create(
      options.packet_store_session_dir, tracks, &packet_store_error);
  if (!session->packet_store.IsWritable()) {
    auto result =
        Result(VideoRecordingBootstrapStatus::PacketStoreCreateFailed,
               WithStagePrefix(L"Video PacketStore creation failed",
                               std::move(packet_store_error)));
    result.encoder_config_result = std::move(encoder_config_result);
    result.encoder_result = std::move(encoder_result);
    result.preflight_result = std::move(preflight_result);
    return result;
  }

  auto prepare_result = session->recording_session->Prepare(
      session->encoder_session.get(), session->d3d.device(),
      session->d3d.immediate_context(), &session->packet_store);
  if (!prepare_result.Succeeded()) {
    auto result =
        Result(VideoRecordingBootstrapStatus::RecordingPrepareFailed,
               prepare_result.message);
    result.encoder_config_result = std::move(encoder_config_result);
    result.encoder_result = std::move(encoder_result);
    result.preflight_result = std::move(preflight_result);
    result.prepare_result = std::move(prepare_result);
    return result;
  }

  auto result = Result(VideoRecordingBootstrapStatus::Success, L"");
  result.encoder_config_result = std::move(encoder_config_result);
  result.encoder_result = std::move(encoder_result);
  result.preflight_result = std::move(preflight_result);
  result.prepare_result = std::move(prepare_result);
  return result;
}

const wchar_t* VideoRecordingBootstrapStatusName(
    VideoRecordingBootstrapStatus status) noexcept {
  switch (status) {
    case VideoRecordingBootstrapStatus::Success:
      return L"success";
    case VideoRecordingBootstrapStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecordingBootstrapStatus::MonitorUnavailable:
      return L"monitor unavailable";
    case VideoRecordingBootstrapStatus::D3DCreateFailed:
      return L"d3d create failed";
    case VideoRecordingBootstrapStatus::EncoderInitFailed:
      return L"encoder init failed";
    case VideoRecordingBootstrapStatus::PacketStoreCreateFailed:
      return L"packet store create failed";
    case VideoRecordingBootstrapStatus::RecordingPreflightFailed:
      return L"recording preflight failed";
    case VideoRecordingBootstrapStatus::RecordingPrepareFailed:
      return L"recording prepare failed";
  }

  return L"unknown";
}

}  // namespace olouie::encode
