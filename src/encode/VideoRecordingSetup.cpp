#include "encode/VideoRecordingSetup.h"

#include <chrono>
#include <string>
#include <utility>

#include "record/Timebase.h"

namespace olouie::encode {
namespace {

constexpr int64_t kNanosecondsPerSecond = 1000000000;

VideoRecordingSetupResult Result(VideoRecordingSetupStatus status,
                                 std::wstring message) {
  VideoRecordingSetupResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool LiveOptionsAreValid(
    const VideoLiveCaptureEncodeOptions& options) noexcept {
  return options.drain_interval > std::chrono::milliseconds(0) &&
         options.max_frames_per_drain_tick > 0 &&
         options.timestamp_frequency > 0 &&
         !(options.start_timebase_on_first_frame &&
           options.use_explicit_timebase_origin) &&
         (!options.use_explicit_timebase_origin ||
          options.timebase_origin_ticks >= 0);
}

int64_t FrameDurationNs(
    const MfHardwareH264EncoderConfig& config) noexcept {
  if (config.fps_numerator == 0) {
    return 0;
  }

  return static_cast<int64_t>(
      (kNanosecondsPerSecond *
       static_cast<int64_t>(config.fps_denominator)) /
      static_cast<int64_t>(config.fps_numerator));
}

VideoRecordingSetupResult ValidatePreflightOptions(
    const VideoRecordingPreflightOptions& options) {
  if (options.video_track_id == 0) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight needs a nonzero video track id.");
  }

  if (options.source_width == 0 || options.source_height == 0) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight needs nonzero source dimensions.");
  }

  if (options.queue_capacity == 0 || options.drain_frame_budget == 0 ||
      options.session_drain_timeout_ms == 0) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight needs nonzero queue capacity, "
                  L"drain budget, and session drain timeout.");
  }

  if (!LiveOptionsAreValid(options.live)) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight live runner options are invalid.");
  }

  return Result(VideoRecordingSetupStatus::Success, L"");
}

}  // namespace

bool VideoRecordingSetupResult::Succeeded() const noexcept {
  return status == VideoRecordingSetupStatus::Success;
}

void VideoRecordingPreflight::Reset() {
  video_track = {};
  encoder_config = {};
  chain_config = {};
  live_options = {};
}

bool VideoRecordingPreflight::IsUsable() const noexcept {
  return video_track.track_id != 0 &&
         video_track.codec_id == record::CodecId::H264 &&
         chain_config.IsValid() && LiveOptionsAreValid(live_options);
}

void VideoRecordingSessionSetup::Reset() {
  chain.reset();
  prepare_result = {};
}

bool VideoRecordingSessionSetup::IsConfigured() const noexcept {
  return chain != nullptr && chain->IsPrepared();
}

VideoRecordingSetupResult BuildVideoRecordingPreflight(
    const VideoRecordingPreflightOptions& options,
    VideoRecordingPreflight* preflight) {
  if (preflight == nullptr) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight needs an output destination.");
  }

  preflight->Reset();

  auto validated = ValidatePreflightOptions(options);
  if (!validated.Succeeded()) {
    return validated;
  }

  auto encoder_config_result =
      ValidateMfHardwareH264EncoderConfig(options.encoder_config);
  if (!encoder_config_result.Succeeded()) {
    auto result = Result(VideoRecordingSetupStatus::EncoderConfigInvalid,
                         encoder_config_result.message);
    result.encoder_config_result = std::move(encoder_config_result);
    return result;
  }

  const int64_t fallback_frame_duration_ns =
      FrameDurationNs(options.encoder_config);
  if (fallback_frame_duration_ns <= 0) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording preflight could not derive frame duration.");
  }

  std::wstring timebase_error;
  const int64_t timebase_origin_ticks =
      options.live.use_explicit_timebase_origin
          ? options.live.timebase_origin_ticks
          : 0;
  auto timebase = record::Timebase::FromQpc(options.live.timestamp_frequency,
                                            timebase_origin_ticks,
                                            &timebase_error);
  if (!timebase.IsValid()) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  std::move(timebase_error));
  }

  preflight->video_track =
      record::TrackDefinition{options.video_track_id, record::CodecId::H264};
  preflight->encoder_config = options.encoder_config;
  preflight->live_options = options.live;
  preflight->chain_config = VideoEncodeChainConfig{
      capture::VideoFrameQueueOptions{options.queue_capacity,
                                      options.overflow_policy},
      BgraVideoRecordingSessionOptions{
          options.video_track_id, options.session_drain_timeout_ms,
          options.source_width, options.source_height},
      VideoEncodeWorkerOptions{timebase, fallback_frame_duration_ns},
      options.drain_frame_budget};

  auto result = Result(VideoRecordingSetupStatus::Success, L"");
  result.encoder_config_result = std::move(encoder_config_result);
  return result;
}

VideoRecordingSetupResult BuildVideoRecordingSessionSetup(
    const VideoRecordingPreflight& preflight,
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    ID3D11DeviceContext* d3d_context,
    record::PacketStore* packet_store,
    VideoRecordingSessionSetup* setup) {
  if (setup == nullptr) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording session setup needs an output destination.");
  }

  setup->Reset();

  if (!preflight.IsUsable()) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording session setup needs a usable preflight.");
  }

  if (encoder_session == nullptr || d3d_device == nullptr ||
      d3d_context == nullptr || packet_store == nullptr ||
      !packet_store->IsWritable()) {
    return Result(VideoRecordingSetupStatus::InvalidConfig,
                  L"Video recording session setup needs a configured encoder "
                  L"session, D3D11 device/context, and writable PacketStore.");
  }

  auto chain = std::make_unique<VideoEncodeChain>(preflight.chain_config);
  setup->prepare_result =
      chain->Prepare(encoder_session, d3d_device, d3d_context, packet_store);
  if (!setup->prepare_result.Succeeded()) {
    auto result = Result(VideoRecordingSetupStatus::SessionCreateFailed,
                         setup->prepare_result.message);
    result.session_prepare_result = setup->prepare_result;
    setup->Reset();
    return result;
  }

  setup->chain = std::move(chain);
  auto result = Result(VideoRecordingSetupStatus::Success, L"");
  result.session_prepare_result = setup->prepare_result;
  return result;
}

const wchar_t* VideoRecordingSetupStatusName(
    VideoRecordingSetupStatus status) noexcept {
  switch (status) {
    case VideoRecordingSetupStatus::Success:
      return L"success";
    case VideoRecordingSetupStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecordingSetupStatus::EncoderConfigInvalid:
      return L"encoder config invalid";
    case VideoRecordingSetupStatus::SessionCreateFailed:
      return L"session create failed";
  }

  return L"unknown";
}

}  // namespace olouie::encode
