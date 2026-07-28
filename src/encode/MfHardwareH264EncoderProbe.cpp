#include "encode/MfHardwareH264EncoderProbe.h"

#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace olouie::encode {
namespace {

class MediaFoundationSession final {
 public:
  MediaFoundationSession() = default;
  ~MediaFoundationSession() {
    if (started_) {
      MFShutdown();
    }
  }

  MediaFoundationSession(const MediaFoundationSession&) = delete;
  MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;

  HRESULT Start() {
    const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    started_ = SUCCEEDED(result);
    return result;
  }

 private:
  bool started_ = false;
};

class ActivateList final {
 public:
  ActivateList() = default;
  ~ActivateList() { Reset(); }

  ActivateList(const ActivateList&) = delete;
  ActivateList& operator=(const ActivateList&) = delete;

  IMFActivate*** put() noexcept {
    Reset();
    return &values_;
  }

  UINT32* count_put() noexcept {
    count_ = 0;
    return &count_;
  }

  IMFActivate* operator[](UINT32 index) const noexcept {
    return values_[index];
  }

  UINT32 count() const noexcept { return count_; }

 private:
  void Reset() noexcept {
    if (values_ != nullptr) {
      for (UINT32 index = 0; index < count_; ++index) {
        if (values_[index] != nullptr) {
          values_[index]->Release();
        }
      }
      CoTaskMemFree(values_);
      values_ = nullptr;
    }
    count_ = 0;
  }

  IMFActivate** values_ = nullptr;
  UINT32 count_ = 0;
};

MfHardwareH264EncoderProbeResult Result(
    MfHardwareH264EncoderProbeStatus status,
    std::wstring message,
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options) {
  MfHardwareH264EncoderProbeResult result;
  result.status = status;
  result.message = std::move(message);
  result.config = config;
  result.options = options;
  return result;
}

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

std::wstring GuidToString(const GUID& guid) {
  wchar_t buffer[39]{};
  const int length = StringFromGUID2(
      guid, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
  if (length <= 1) {
    return {};
  }
  return std::wstring(buffer, static_cast<size_t>(length - 1));
}

std::wstring ActivateString(IMFActivate* activate, const GUID& key) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  if (activate != nullptr &&
      SUCCEEDED(activate->GetAllocatedString(key, &value, &length)) &&
      value != nullptr) {
    std::wstring text(value, length);
    CoTaskMemFree(value);
    return text;
  }

  return {};
}

MfHardwareH264EncoderInfo InfoFromActivate(IMFActivate* activate,
                                           uint32_t flags) {
  MfHardwareH264EncoderInfo info;
  info.name = ActivateString(activate, MFT_FRIENDLY_NAME_Attribute);
  if (info.name.empty()) {
    info.name = L"Media Foundation hardware H.264 encoder";
  }

  GUID clsid{};
  if (activate != nullptr &&
      SUCCEEDED(activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
    info.clsid = GuidToString(clsid);
  }

  info.enumeration_flags = flags;
  return info;
}

uint32_t EnumerationFlags(
    const MfHardwareH264EncoderProbeOptions& options) noexcept {
  uint32_t flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
  if (options.include_local_mfts) {
    flags |= MFT_ENUM_FLAG_LOCALMFT;
  }
  return flags;
}

}  // namespace

bool MfHardwareH264EncoderProbeResult::Succeeded() const noexcept {
  return status == MfHardwareH264EncoderProbeStatus::Success;
}

const MfHardwareH264EncoderInfo*
MfHardwareH264EncoderProbeResult::selected_encoder() const noexcept {
  if (selected_encoder_index >= encoders.size()) {
    return nullptr;
  }
  return &encoders[selected_encoder_index];
}

bool MfHardwareH264PerformanceTuningPlan::IsValid() const noexcept {
  return performance::IsValidCapturePerformanceMode(mode) &&
         requested_quality_vs_speed <= 100;
}

MfHardwareH264PerformanceTuningPlan
BuildMfHardwareH264PerformanceTuningPlan(
    performance::CapturePerformanceMode mode) {
  MfHardwareH264PerformanceTuningPlan plan;
  plan.mode = mode;
  const bool capture_first =
      mode == performance::CapturePerformanceMode::CaptureFirst;
  plan.apply_low_latency = capture_first;
  plan.requested_low_latency = capture_first;
  plan.apply_quality_vs_speed = capture_first;
  plan.requested_quality_vs_speed = 0;
  return plan;
}

MfHardwareH264EncoderProbeResult ValidateMfHardwareH264EncoderConfig(
    const MfHardwareH264EncoderConfig& config) {
  MfHardwareH264EncoderProbeOptions options;

  if (!performance::IsValidCapturePerformanceMode(
          config.performance_mode)) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder performance mode is invalid.", config,
                  options);
  }

  if (config.width == 0 || config.height == 0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder dimensions must be nonzero.", config,
                  options);
  }

  if ((config.width % 2) != 0 || (config.height % 2) != 0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264/NV12 encoder dimensions must be even.", config,
                  options);
  }

  if (config.fps_numerator == 0 || config.fps_denominator == 0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder FPS ratio must be nonzero.", config,
                  options);
  }

  if (config.bitrate_bps == 0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder bitrate must be greater than zero.", config,
                  options);
  }

  if (!std::isfinite(config.gop_seconds) || config.gop_seconds <= 0.0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder GOP seconds must be positive.", config,
                  options);
  }

  if (config.max_b_frames != 0) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 B-frames are deferred for MVP timestamp "
                  L"simplicity.",
                  config, options);
  }

  return Result(MfHardwareH264EncoderProbeStatus::Success, L"", config,
                options);
}

MfHardwareH264EncoderProbeResult ProbeMfHardwareH264Encoder(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options) {
  return ProbeMfHardwareH264Encoder(config, options,
                                    &EnumerateMfHardwareH264Encoders);
}

MfHardwareH264EncoderProbeResult ProbeMfHardwareH264Encoder(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options,
    MfHardwareH264EncoderEnumerator enumerator) {
  auto validation = ValidateMfHardwareH264EncoderConfig(config);
  validation.options = options;
  if (!validation.Succeeded()) {
    return validation;
  }

  if (enumerator == nullptr) {
    return Result(MfHardwareH264EncoderProbeStatus::InvalidConfig,
                  L"H.264 encoder probe needs an enumerator.", config,
                  options);
  }

  std::vector<MfHardwareH264EncoderInfo> encoders;
  std::wstring message;
  const auto status = enumerator(options, &encoders, &message);
  if (status != MfHardwareH264EncoderProbeStatus::Success) {
    return Result(status, std::move(message), config, options);
  }

  auto result =
      Result(MfHardwareH264EncoderProbeStatus::Success, L"", config, options);
  result.encoders = std::move(encoders);
  if (result.encoders.empty()) {
    result.status =
        MfHardwareH264EncoderProbeStatus::HardwareEncoderUnavailable;
    result.message =
        L"No Media Foundation hardware H.264 encoder MFT is available for "
        L"NV12 input and H.264 output.";
    return result;
  }

  result.selected_encoder_index = 0;
  return result;
}

MfHardwareH264EncoderProbeStatus EnumerateMfHardwareH264Encoders(
    const MfHardwareH264EncoderProbeOptions& options,
    std::vector<MfHardwareH264EncoderInfo>* encoders,
    std::wstring* message) {
  if (encoders == nullptr) {
    if (message != nullptr) {
      *message = L"H.264 encoder enumeration needs an output destination.";
    }
    return MfHardwareH264EncoderProbeStatus::InvalidConfig;
  }
  encoders->clear();

  MediaFoundationSession media_foundation;
  HRESULT result = media_foundation.Start();
  if (FAILED(result)) {
    if (message != nullptr) {
      *message = L"Could not start Media Foundation (" +
                 HResultToHex(result) + L").";
    }
    return MfHardwareH264EncoderProbeStatus::MediaFoundationUnavailable;
  }

  MFT_REGISTER_TYPE_INFO input_info;
  input_info.guidMajorType = MFMediaType_Video;
  input_info.guidSubtype = MFVideoFormat_NV12;

  MFT_REGISTER_TYPE_INFO output_info;
  output_info.guidMajorType = MFMediaType_Video;
  output_info.guidSubtype = MFVideoFormat_H264;

  const uint32_t flags = EnumerationFlags(options);
  ActivateList activates;
  result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info,
                     &output_info, activates.put(), activates.count_put());
  if (FAILED(result)) {
    if (message != nullptr) {
      *message = L"Could not enumerate hardware H.264 encoder MFTs (" +
                 HResultToHex(result) + L").";
    }
    return MfHardwareH264EncoderProbeStatus::EnumerationFailed;
  }

  encoders->reserve(activates.count());
  for (UINT32 index = 0; index < activates.count(); ++index) {
    encoders->push_back(InfoFromActivate(activates[index], flags));
  }

  if (message != nullptr) {
    message->clear();
  }
  return MfHardwareH264EncoderProbeStatus::Success;
}

const wchar_t* MfHardwareH264EncoderProbeStatusName(
    MfHardwareH264EncoderProbeStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderProbeStatus::Success:
      return L"success";
    case MfHardwareH264EncoderProbeStatus::InvalidConfig:
      return L"invalid config";
    case MfHardwareH264EncoderProbeStatus::MediaFoundationUnavailable:
      return L"media foundation unavailable";
    case MfHardwareH264EncoderProbeStatus::EnumerationFailed:
      return L"enumeration failed";
    case MfHardwareH264EncoderProbeStatus::HardwareEncoderUnavailable:
      return L"hardware encoder unavailable";
  }

  return L"unknown";
}

}  // namespace olouie::encode
