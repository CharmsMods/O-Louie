#pragma once

#include <cstdint>
#include <string>

#include "encode/H264PacketStore.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "graphics/GpuBgraToNv12.h"
#include "record/PacketStore.h"

#include <winrt/base.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace olouie::encode {

enum class VideoRecordingSessionStatus {
  Success,
  InvalidConfig,
  InvalidState,
  ConverterInitFailed,
  TextureCreateFailed,
  ConvertFailed,
  SubmitFailed,
  DrainFailed,
  ConfigUnavailable,
  PacketStoreFailed,
};

enum class VideoRecordingRuntimeFaultKind {
  None,
  D3D11DeviceLost,
  HardwareEncoderFailed,
};

struct SyntheticVideoRecordingSessionOptions {
  uint32_t video_track_id = 1;
  uint32_t drain_timeout_ms = 3000;
};

struct SyntheticVideoRecordingSessionStats {
  uint64_t submitted_frame_count = 0;
  uint64_t drained_packet_count = 0;
  uint64_t appended_packet_count = 0;
  uint64_t encoder_wait_count = 0;
  uint64_t last_encoder_wait_ns = 0;
  uint64_t maximum_encoder_wait_ns = 0;
  uint64_t total_encoder_wait_ns = 0;
};

struct SyntheticVideoRecordingSessionResult {
  VideoRecordingSessionStatus status = VideoRecordingSessionStatus::InvalidConfig;
  std::wstring message;
  MfHardwareH264EncoderFrameSubmitResult submit_result;
  MfHardwareH264EncoderDrainResult drain_result;

  bool Succeeded() const noexcept;
};

struct BgraVideoRecordingSessionOptions {
  uint32_t video_track_id = 1;
  uint32_t drain_timeout_ms = 3000;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
};

struct BgraVideoRecordingSessionStats {
  uint64_t converted_frame_count = 0;
  uint64_t submitted_frame_count = 0;
  uint64_t drained_packet_count = 0;
  uint64_t appended_packet_count = 0;
  uint64_t encoder_wait_count = 0;
  uint64_t last_encoder_wait_ns = 0;
  uint64_t maximum_encoder_wait_ns = 0;
  uint64_t total_encoder_wait_ns = 0;
};

struct BgraVideoRecordingSessionResult {
  VideoRecordingSessionStatus status = VideoRecordingSessionStatus::InvalidConfig;
  std::wstring message;
  graphics::GpuBgraToNv12ConvertResult convert_result;
  MfHardwareH264EncoderFrameSubmitResult submit_result;
  MfHardwareH264EncoderDrainResult drain_result;

  bool Succeeded() const noexcept;
  VideoRecordingRuntimeFaultKind RuntimeFaultKind() const noexcept;
  bool RequiresPreservationFinalization() const noexcept;
};

class SyntheticVideoRecordingSession final {
 public:
  explicit SyntheticVideoRecordingSession(
      SyntheticVideoRecordingSessionOptions options);

  SyntheticVideoRecordingSessionResult Prepare(
      MfHardwareH264EncoderSession* encoder_session,
      ID3D11Device* d3d_device,
      record::PacketStore* packet_store);
  SyntheticVideoRecordingSessionResult SubmitGeneratedFrame(
      int64_t pts_ns,
      int64_t duration_ns);
  void Reset();

  bool IsPrepared() const noexcept;
  const SyntheticVideoRecordingSessionOptions& options() const noexcept;
  const SyntheticVideoRecordingSessionStats& stats() const noexcept;
  const H264PacketStoreConfig& config() const noexcept;
  const SyntheticVideoRecordingSessionResult& last_result() const noexcept;

 private:
  SyntheticVideoRecordingSessionResult Result(VideoRecordingSessionStatus status,
                                              std::wstring message) const;
  SyntheticVideoRecordingSessionResult AppendDrainedPackets(
      MfHardwareH264EncoderDrainResult drain_result);

  SyntheticVideoRecordingSessionOptions options_;
  MfHardwareH264EncoderSession* encoder_session_ = nullptr;
  ID3D11Device* d3d_device_ = nullptr;
  record::PacketStore* packet_store_ = nullptr;
  H264PacketStoreConfig config_;
  SyntheticVideoRecordingSessionStats stats_;
  SyntheticVideoRecordingSessionResult last_result_;
};

class BgraVideoRecordingSession final {
 public:
  explicit BgraVideoRecordingSession(
      BgraVideoRecordingSessionOptions options);

  BgraVideoRecordingSessionResult Prepare(
      MfHardwareH264EncoderSession* encoder_session,
      ID3D11Device* d3d_device,
      ID3D11DeviceContext* d3d_context,
      record::PacketStore* packet_store);
  BgraVideoRecordingSessionResult SubmitBgraFrame(ID3D11Texture2D* source_bgra,
                                                  int64_t pts_ns,
                                                  int64_t duration_ns);
  BgraVideoRecordingSessionResult Finalize();
  void Reset();

  bool IsPrepared() const noexcept;
  const BgraVideoRecordingSessionOptions& options() const noexcept;
  const BgraVideoRecordingSessionStats& stats() const noexcept;
  const H264PacketStoreConfig& config() const noexcept;
  const graphics::GpuBgraToNv12Plan& conversion_plan() const noexcept;
  const graphics::GpuBgraToNv12ConverterStats& converter_stats()
      const noexcept;
  MfHardwareH264EncoderSessionInfo encoder_info_snapshot() const;
  ID3D11Texture2D* nv12_texture() const noexcept;
  const BgraVideoRecordingSessionResult& last_result() const noexcept;

 private:
  BgraVideoRecordingSessionResult Result(VideoRecordingSessionStatus status,
                                         std::wstring message) const;
  BgraVideoRecordingSessionResult AppendDrainedPackets(
      MfHardwareH264EncoderDrainResult drain_result);

  BgraVideoRecordingSessionOptions options_;
  MfHardwareH264EncoderSession* encoder_session_ = nullptr;
  ID3D11Device* d3d_device_ = nullptr;
  ID3D11DeviceContext* d3d_context_ = nullptr;
  record::PacketStore* packet_store_ = nullptr;
  graphics::GpuBgraToNv12Converter converter_;
  winrt::com_ptr<ID3D11Texture2D> nv12_texture_;
  H264PacketStoreConfig config_;
  BgraVideoRecordingSessionStats stats_;
  BgraVideoRecordingSessionResult last_result_;
};

const wchar_t* VideoRecordingSessionStatusName(
    VideoRecordingSessionStatus status) noexcept;
const wchar_t* VideoRecordingRuntimeFaultKindName(
    VideoRecordingRuntimeFaultKind kind) noexcept;

}  // namespace olouie::encode
