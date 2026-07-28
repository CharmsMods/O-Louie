#include "encode/VideoRecordingSession.h"

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace olouie::encode {
namespace {

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

}  // namespace

bool SyntheticVideoRecordingSessionResult::Succeeded() const noexcept {
  return status == VideoRecordingSessionStatus::Success;
}

bool BgraVideoRecordingSessionResult::Succeeded() const noexcept {
  return status == VideoRecordingSessionStatus::Success;
}

VideoRecordingRuntimeFaultKind
BgraVideoRecordingSessionResult::RuntimeFaultKind() const noexcept {
  if (convert_result.device_fault.Failed() ||
      submit_result.device_fault.Failed() ||
      drain_result.device_fault.Failed()) {
    return VideoRecordingRuntimeFaultKind::D3D11DeviceLost;
  }
  if (IsMfHardwareH264EncoderRuntimeFailure(submit_result.status) ||
      IsMfHardwareH264EncoderRuntimeFailure(drain_result.status)) {
    return VideoRecordingRuntimeFaultKind::HardwareEncoderFailed;
  }
  return VideoRecordingRuntimeFaultKind::None;
}

bool BgraVideoRecordingSessionResult::RequiresPreservationFinalization()
    const noexcept {
  return RuntimeFaultKind() != VideoRecordingRuntimeFaultKind::None;
}

SyntheticVideoRecordingSession::SyntheticVideoRecordingSession(
    SyntheticVideoRecordingSessionOptions options)
    : options_(options) {}

SyntheticVideoRecordingSessionResult SyntheticVideoRecordingSession::Prepare(
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    record::PacketStore* packet_store) {
  Reset();

  if (options_.video_track_id == 0 || options_.drain_timeout_ms == 0) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidConfig,
                          L"Video recording session needs a nonzero track id "
                          L"and drain timeout.");
    return last_result_;
  }
  if (encoder_session == nullptr || !encoder_session->IsConfigured() ||
      d3d_device == nullptr || packet_store == nullptr ||
      !packet_store->IsWritable()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"Video recording session needs a configured encoder, "
                          L"D3D11 device, and writable PacketStore.");
    return last_result_;
  }

  encoder_session_ = encoder_session;
  d3d_device_ = d3d_device;
  packet_store_ = packet_store;
  last_result_ = Result(VideoRecordingSessionStatus::Success, L"");
  return last_result_;
}

SyntheticVideoRecordingSessionResult
SyntheticVideoRecordingSession::SubmitGeneratedFrame(int64_t pts_ns,
                                                     int64_t duration_ns) {
  if (!IsPrepared()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"Video recording session must be prepared before "
                          L"submitting frames.");
    return last_result_;
  }

  auto submit = encoder_session_->SubmitSyntheticNv12Frame(
      d3d_device_, pts_ns, duration_ns);
  if (!submit.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::SubmitFailed,
                          submit.message);
    last_result_.submit_result = std::move(submit);
    return last_result_;
  }
  ++stats_.submitted_frame_count;

  const auto encoder_wait_started = std::chrono::steady_clock::now();
  auto drain = encoder_session_->DrainSyntheticAvailableOutput(
      options_.drain_timeout_ms);
  const auto encoder_wait_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - encoder_wait_started)
          .count());
  ++stats_.encoder_wait_count;
  stats_.last_encoder_wait_ns = encoder_wait_ns;
  stats_.maximum_encoder_wait_ns =
      std::max(stats_.maximum_encoder_wait_ns, encoder_wait_ns);
  stats_.total_encoder_wait_ns += encoder_wait_ns;
  if (!drain.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::DrainFailed,
                          drain.message);
    last_result_.submit_result = std::move(submit);
    last_result_.drain_result = std::move(drain);
    return last_result_;
  }

  auto result = AppendDrainedPackets(std::move(drain));
  result.submit_result = std::move(submit);
  last_result_ = std::move(result);
  return last_result_;
}

void SyntheticVideoRecordingSession::Reset() {
  encoder_session_ = nullptr;
  d3d_device_ = nullptr;
  packet_store_ = nullptr;
  config_ = {};
  stats_ = {};
  last_result_ = {};
}

bool SyntheticVideoRecordingSession::IsPrepared() const noexcept {
  return encoder_session_ != nullptr && d3d_device_ != nullptr &&
         packet_store_ != nullptr;
}

const SyntheticVideoRecordingSessionOptions&
SyntheticVideoRecordingSession::options() const noexcept {
  return options_;
}

const SyntheticVideoRecordingSessionStats&
SyntheticVideoRecordingSession::stats() const noexcept {
  return stats_;
}

const H264PacketStoreConfig& SyntheticVideoRecordingSession::config()
    const noexcept {
  return config_;
}

const SyntheticVideoRecordingSessionResult&
SyntheticVideoRecordingSession::last_result() const noexcept {
  return last_result_;
}

SyntheticVideoRecordingSessionResult SyntheticVideoRecordingSession::Result(
    VideoRecordingSessionStatus status,
    std::wstring message) const {
  SyntheticVideoRecordingSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

SyntheticVideoRecordingSessionResult
SyntheticVideoRecordingSession::AppendDrainedPackets(
    MfHardwareH264EncoderDrainResult drain_result) {
  std::wstring error;
  for (const auto& packet : drain_result.packets) {
    ++stats_.drained_packet_count;
    if (!config_.IsReady() &&
        packet.bitstream.config.HasAvccExtradata() &&
        !BuildH264PacketStoreConfig(options_.video_track_id,
                                    packet.bitstream.config, &config_,
                                    &error)) {
      auto result =
          Result(VideoRecordingSessionStatus::ConfigUnavailable, error);
      result.drain_result = std::move(drain_result);
      return result;
    }

    if (!AppendH264Packet(packet_store_, options_.video_track_id, packet,
                          &error)) {
      auto result =
          Result(VideoRecordingSessionStatus::PacketStoreFailed, error);
      result.drain_result = std::move(drain_result);
      return result;
    }
    ++stats_.appended_packet_count;
  }

  if (!config_.IsReady()) {
    auto result = Result(VideoRecordingSessionStatus::ConfigUnavailable,
                         L"Video recording session has not received H.264 "
                         L"SPS/PPS/AVCC config.");
    result.drain_result = std::move(drain_result);
    return result;
  }

  auto result = Result(VideoRecordingSessionStatus::Success, L"");
  result.drain_result = std::move(drain_result);
  return result;
}

BgraVideoRecordingSession::BgraVideoRecordingSession(
    BgraVideoRecordingSessionOptions options)
    : options_(options) {}

BgraVideoRecordingSessionResult BgraVideoRecordingSession::Prepare(
    MfHardwareH264EncoderSession* encoder_session,
    ID3D11Device* d3d_device,
    ID3D11DeviceContext* d3d_context,
    record::PacketStore* packet_store) {
  Reset();

  if (options_.video_track_id == 0 || options_.drain_timeout_ms == 0 ||
      options_.source_width == 0 || options_.source_height == 0) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidConfig,
                          L"BGRA video session needs a nonzero track id, "
                          L"drain timeout, and source dimensions.");
    return last_result_;
  }
  if (encoder_session == nullptr || !encoder_session->IsConfigured() ||
      d3d_device == nullptr || d3d_context == nullptr ||
      packet_store == nullptr || !packet_store->IsWritable()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"BGRA video session needs a configured encoder, "
                          L"D3D11 device/context, and writable PacketStore.");
    return last_result_;
  }

  const auto& media_type = encoder_session->info().media_type;
  if (!media_type.IsValid()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"BGRA video session needs a valid H.264 encoder "
                          L"media type.");
    return last_result_;
  }

  const graphics::GpuBgraToNv12Config convert_config{
      options_.source_width,
      options_.source_height,
      media_type.width,
      media_type.height,
  };
  auto convert_init =
      converter_.Initialize(d3d_device, d3d_context, convert_config);
  if (!convert_init.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::ConverterInitFailed,
                          convert_init.message);
    last_result_.convert_result = std::move(convert_init);
    return last_result_;
  }

  D3D11_TEXTURE2D_DESC output_desc{};
  output_desc.Width = media_type.width;
  output_desc.Height = media_type.height;
  output_desc.MipLevels = 1;
  output_desc.ArraySize = 1;
  output_desc.Format = DXGI_FORMAT_NV12;
  output_desc.SampleDesc.Count = 1;
  output_desc.Usage = D3D11_USAGE_DEFAULT;
  output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  HRESULT hr =
      d3d_device->CreateTexture2D(&output_desc, nullptr, nv12_texture_.put());
  if (FAILED(hr)) {
    last_result_ = Result(VideoRecordingSessionStatus::TextureCreateFailed,
                          L"Could not create reusable encoder NV12 texture (" +
                              HResultToHex(hr) + L").");
    return last_result_;
  }

  encoder_session_ = encoder_session;
  d3d_device_ = d3d_device;
  d3d_context_ = d3d_context;
  packet_store_ = packet_store;
  last_result_ = Result(VideoRecordingSessionStatus::Success, L"");
  return last_result_;
}

BgraVideoRecordingSessionResult BgraVideoRecordingSession::SubmitBgraFrame(
    ID3D11Texture2D* source_bgra,
    int64_t pts_ns,
    int64_t duration_ns) {
  if (!IsPrepared()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"BGRA video session must be prepared before "
                          L"submitting frames.");
    return last_result_;
  }

  auto convert = converter_.Convert(source_bgra, nv12_texture_.get());
  if (!convert.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::ConvertFailed,
                          convert.message);
    last_result_.convert_result = std::move(convert);
    return last_result_;
  }
  ++stats_.converted_frame_count;

  auto submit = encoder_session_->SubmitNv12Texture(nv12_texture_.get(),
                                                    pts_ns, duration_ns);
  if (!submit.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::SubmitFailed,
                          submit.message);
    last_result_.convert_result = std::move(convert);
    last_result_.submit_result = std::move(submit);
    return last_result_;
  }
  ++stats_.submitted_frame_count;

  const auto encoder_wait_started = std::chrono::steady_clock::now();
  auto drain = encoder_session_->DrainSyntheticAvailableOutput(
      options_.drain_timeout_ms);
  const auto encoder_wait_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - encoder_wait_started)
          .count());
  ++stats_.encoder_wait_count;
  stats_.last_encoder_wait_ns = encoder_wait_ns;
  stats_.maximum_encoder_wait_ns =
      std::max(stats_.maximum_encoder_wait_ns, encoder_wait_ns);
  stats_.total_encoder_wait_ns += encoder_wait_ns;
  if (!drain.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::DrainFailed,
                          drain.message);
    last_result_.convert_result = std::move(convert);
    last_result_.submit_result = std::move(submit);
    last_result_.drain_result = std::move(drain);
    return last_result_;
  }

  auto result = AppendDrainedPackets(std::move(drain));
  result.convert_result = std::move(convert);
  result.submit_result = std::move(submit);
  last_result_ = std::move(result);
  return last_result_;
}

BgraVideoRecordingSessionResult BgraVideoRecordingSession::Finalize() {
  if (!IsPrepared()) {
    last_result_ = Result(VideoRecordingSessionStatus::InvalidState,
                          L"BGRA video session must be prepared before final "
                          L"encoder draining.");
    return last_result_;
  }

  const auto encoder_wait_started = std::chrono::steady_clock::now();
  auto drain = encoder_session_->DrainSyntheticEncodedOutput(
      options_.drain_timeout_ms);
  const auto encoder_wait_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - encoder_wait_started)
          .count());
  ++stats_.encoder_wait_count;
  stats_.last_encoder_wait_ns = encoder_wait_ns;
  stats_.maximum_encoder_wait_ns =
      std::max(stats_.maximum_encoder_wait_ns, encoder_wait_ns);
  stats_.total_encoder_wait_ns += encoder_wait_ns;
  if (!drain.Succeeded()) {
    last_result_ = Result(VideoRecordingSessionStatus::DrainFailed,
                          drain.message);
    last_result_.drain_result = std::move(drain);
    return last_result_;
  }

  last_result_ = AppendDrainedPackets(std::move(drain));
  return last_result_;
}

void BgraVideoRecordingSession::Reset() {
  encoder_session_ = nullptr;
  d3d_device_ = nullptr;
  d3d_context_ = nullptr;
  packet_store_ = nullptr;
  converter_ = graphics::GpuBgraToNv12Converter();
  nv12_texture_ = nullptr;
  config_ = {};
  stats_ = {};
  last_result_ = {};
}

bool BgraVideoRecordingSession::IsPrepared() const noexcept {
  return encoder_session_ != nullptr && d3d_device_ != nullptr &&
         d3d_context_ != nullptr && packet_store_ != nullptr &&
         converter_.IsInitialized() && nv12_texture_ != nullptr;
}

const BgraVideoRecordingSessionOptions& BgraVideoRecordingSession::options()
    const noexcept {
  return options_;
}

const BgraVideoRecordingSessionStats& BgraVideoRecordingSession::stats()
    const noexcept {
  return stats_;
}

const H264PacketStoreConfig& BgraVideoRecordingSession::config()
    const noexcept {
  return config_;
}

const graphics::GpuBgraToNv12Plan&
BgraVideoRecordingSession::conversion_plan() const noexcept {
  return converter_.plan();
}

const graphics::GpuBgraToNv12ConverterStats&
BgraVideoRecordingSession::converter_stats() const noexcept {
  return converter_.stats();
}

MfHardwareH264EncoderSessionInfo
BgraVideoRecordingSession::encoder_info_snapshot() const {
  return encoder_session_ == nullptr ? MfHardwareH264EncoderSessionInfo{}
                                     : encoder_session_->info();
}

ID3D11Texture2D* BgraVideoRecordingSession::nv12_texture() const noexcept {
  return nv12_texture_.get();
}

const BgraVideoRecordingSessionResult& BgraVideoRecordingSession::last_result()
    const noexcept {
  return last_result_;
}

BgraVideoRecordingSessionResult BgraVideoRecordingSession::Result(
    VideoRecordingSessionStatus status,
    std::wstring message) const {
  BgraVideoRecordingSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

BgraVideoRecordingSessionResult
BgraVideoRecordingSession::AppendDrainedPackets(
    MfHardwareH264EncoderDrainResult drain_result) {
  std::wstring error;
  for (const auto& packet : drain_result.packets) {
    ++stats_.drained_packet_count;
    if (!config_.IsReady() &&
        packet.bitstream.config.HasAvccExtradata() &&
        !BuildH264PacketStoreConfig(options_.video_track_id,
                                    packet.bitstream.config, &config_,
                                    &error)) {
      auto result =
          Result(VideoRecordingSessionStatus::ConfigUnavailable, error);
      result.drain_result = std::move(drain_result);
      return result;
    }

    if (!AppendH264Packet(packet_store_, options_.video_track_id, packet,
                          &error)) {
      auto result =
          Result(VideoRecordingSessionStatus::PacketStoreFailed, error);
      result.drain_result = std::move(drain_result);
      return result;
    }
    ++stats_.appended_packet_count;
  }

  if (!config_.IsReady()) {
    auto result = Result(VideoRecordingSessionStatus::ConfigUnavailable,
                         L"BGRA video session has not received H.264 "
                         L"SPS/PPS/AVCC config.");
    result.drain_result = std::move(drain_result);
    return result;
  }

  auto result = Result(VideoRecordingSessionStatus::Success, L"");
  result.drain_result = std::move(drain_result);
  return result;
}

const wchar_t* VideoRecordingSessionStatusName(
    VideoRecordingSessionStatus status) noexcept {
  switch (status) {
    case VideoRecordingSessionStatus::Success:
      return L"success";
    case VideoRecordingSessionStatus::InvalidConfig:
      return L"invalid config";
    case VideoRecordingSessionStatus::InvalidState:
      return L"invalid state";
    case VideoRecordingSessionStatus::ConverterInitFailed:
      return L"converter init failed";
    case VideoRecordingSessionStatus::TextureCreateFailed:
      return L"texture create failed";
    case VideoRecordingSessionStatus::ConvertFailed:
      return L"convert failed";
    case VideoRecordingSessionStatus::SubmitFailed:
      return L"submit failed";
    case VideoRecordingSessionStatus::DrainFailed:
      return L"drain failed";
    case VideoRecordingSessionStatus::ConfigUnavailable:
      return L"config unavailable";
    case VideoRecordingSessionStatus::PacketStoreFailed:
      return L"packet store failed";
  }

  return L"unknown";
}

const wchar_t* VideoRecordingRuntimeFaultKindName(
    VideoRecordingRuntimeFaultKind kind) noexcept {
  switch (kind) {
    case VideoRecordingRuntimeFaultKind::None:
      return L"none";
    case VideoRecordingRuntimeFaultKind::D3D11DeviceLost:
      return L"D3D11 device lost";
    case VideoRecordingRuntimeFaultKind::HardwareEncoderFailed:
      return L"hardware encoder failed";
  }
  return L"unknown";
}

}  // namespace olouie::encode
