#pragma once

#include <windows.h>

#include <chrono>
#include <string>

#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoLiveCaptureEncode.h"
#include "encode/VideoRecordingSetup.h"
#include "record/PacketStore.h"

namespace olouie::encode {

enum class VideoRecordingRunSessionStatus {
  Success,
  InvalidConfig,
  InvalidState,
  PreflightFailed,
  SetupFailed,
  RunFailed,
};

struct VideoRecordingRunSessionOptions {
  HMONITOR monitor = nullptr;
  VideoRecordingPreflightOptions preflight;
  WgcVideoFrameCopyRunner capture_runner = nullptr;
};

struct VideoRecordingRunSessionResult {
  VideoRecordingRunSessionStatus status =
      VideoRecordingRunSessionStatus::InvalidConfig;
  std::wstring message;
  VideoRecordingSetupResult setup_result;
  VideoLiveCaptureEncodeRunResult live_result;

  bool Succeeded() const noexcept;
};

class VideoRecordingRunSession final {
 public:
  explicit VideoRecordingRunSession(VideoRecordingRunSessionOptions options);

  VideoRecordingRunSessionResult Preflight();
  VideoRecordingRunSessionResult Prepare(
      MfHardwareH264EncoderSession* encoder_session,
      ID3D11Device* d3d_device,
      ID3D11DeviceContext* d3d_context,
      record::PacketStore* packet_store);
  VideoRecordingRunSessionResult Run();
  VideoRecordingRunSessionResult RunForDuration(
      std::chrono::milliseconds duration);
  void Reset();

  bool IsPreflighted() const noexcept;
  bool IsPrepared() const noexcept;

  const VideoRecordingRunSessionOptions& options() const noexcept;
  const VideoRecordingPreflight& preflight() const noexcept;
  const VideoRecordingSessionSetup& setup() const noexcept;
  VideoEncodeChain* encode_chain() const noexcept;
  MfHardwareH264EncoderSession* encoder_session() const noexcept;
  record::PacketStore* packet_store() const noexcept;
  const VideoLiveCaptureEncodeResult& last_live_result() const noexcept;
  const VideoRecordingRunSessionResult& last_result() const noexcept;

 private:
  VideoRecordingRunSessionOptions options_;
  MfHardwareH264EncoderSession* encoder_session_ = nullptr;
  ID3D11Device* d3d_device_ = nullptr;
  ID3D11DeviceContext* d3d_context_ = nullptr;
  record::PacketStore* packet_store_ = nullptr;
  VideoRecordingPreflight preflight_;
  VideoRecordingSessionSetup setup_;
  VideoLiveCaptureEncodeResult last_live_result_;
  VideoRecordingRunSessionResult last_result_;
};

const wchar_t* VideoRecordingRunSessionStatusName(
    VideoRecordingRunSessionStatus status) noexcept;

}  // namespace olouie::encode
