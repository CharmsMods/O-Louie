#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "performance/CapturePerformance.h"

namespace olouie::encode {

enum class MfHardwareH264EncoderProbeStatus {
  Success,
  InvalidConfig,
  MediaFoundationUnavailable,
  EnumerationFailed,
  HardwareEncoderUnavailable,
};

struct MfHardwareH264EncoderConfig {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fps_numerator = 60;
  uint32_t fps_denominator = 1;
  uint32_t bitrate_bps = 20000000;
  double gop_seconds = 2.0;
  uint32_t max_b_frames = 0;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
};

struct MfHardwareH264PerformanceTuningPlan {
  performance::CapturePerformanceMode mode =
      performance::CapturePerformanceMode::Balanced;
  bool apply_low_latency = false;
  bool requested_low_latency = false;
  bool apply_quality_vs_speed = false;
  uint32_t requested_quality_vs_speed = 0;

  bool IsValid() const noexcept;
};

struct MfHardwareH264EncoderProbeOptions {
  bool include_local_mfts = true;
};

struct MfHardwareH264EncoderInfo {
  std::wstring name;
  std::wstring clsid;
  uint32_t enumeration_flags = 0;
};

struct MfHardwareH264EncoderProbeResult {
  MfHardwareH264EncoderProbeStatus status =
      MfHardwareH264EncoderProbeStatus::InvalidConfig;
  std::wstring message;
  MfHardwareH264EncoderConfig config;
  MfHardwareH264EncoderProbeOptions options;
  std::vector<MfHardwareH264EncoderInfo> encoders;
  size_t selected_encoder_index = static_cast<size_t>(-1);

  bool Succeeded() const noexcept;
  const MfHardwareH264EncoderInfo* selected_encoder() const noexcept;
};

using MfHardwareH264EncoderEnumerator =
    MfHardwareH264EncoderProbeStatus (*)(
        const MfHardwareH264EncoderProbeOptions& options,
        std::vector<MfHardwareH264EncoderInfo>* encoders,
        std::wstring* message);

MfHardwareH264EncoderProbeResult ValidateMfHardwareH264EncoderConfig(
    const MfHardwareH264EncoderConfig& config);

MfHardwareH264PerformanceTuningPlan
BuildMfHardwareH264PerformanceTuningPlan(
    performance::CapturePerformanceMode mode);

MfHardwareH264EncoderProbeResult ProbeMfHardwareH264Encoder(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options);

MfHardwareH264EncoderProbeResult ProbeMfHardwareH264Encoder(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options,
    MfHardwareH264EncoderEnumerator enumerator);

MfHardwareH264EncoderProbeStatus EnumerateMfHardwareH264Encoders(
    const MfHardwareH264EncoderProbeOptions& options,
    std::vector<MfHardwareH264EncoderInfo>* encoders,
    std::wstring* message);

const wchar_t* MfHardwareH264EncoderProbeStatusName(
    MfHardwareH264EncoderProbeStatus status) noexcept;

}  // namespace olouie::encode
