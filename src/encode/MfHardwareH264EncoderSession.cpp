#include "encode/MfHardwareH264EncoderSession.h"

#include <codecapi.h>
#include <combaseapi.h>
#include <d3d11.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace olouie::encode {
namespace {

template <typename T>
class ComPtr final {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T* get() const noexcept { return value_; }
  T** put() noexcept {
    Reset();
    return &value_;
  }

  T* Detach() noexcept {
    T* detached = value_;
    value_ = nullptr;
    return detached;
  }

  void Reset() noexcept {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  T* value_ = nullptr;
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

MfHardwareH264EncoderSessionResult Result(
    MfHardwareH264EncoderSessionStatus status,
    std::wstring message) {
  MfHardwareH264EncoderSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

MfHardwareH264EncoderFrameSubmitResult FrameSubmitResult(
    MfHardwareH264EncoderFrameSubmitStatus status,
    std::wstring message,
    HRESULT hresult = S_OK,
    ID3D11Device* d3d_device = nullptr,
    std::wstring operation = {}) {
  MfHardwareH264EncoderFrameSubmitResult result;
  result.status = status;
  result.message = std::move(message);
  result.hresult = hresult;
  result.device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device, hresult, std::move(operation));
  if (result.device_fault.Failed()) {
    result.message = result.device_fault.message;
  }
  return result;
}

MfHardwareH264EncoderDrainResult DrainResult(
    MfHardwareH264EncoderDrainStatus status,
    std::wstring message,
    HRESULT hresult = S_OK,
    ID3D11Device* d3d_device = nullptr,
    std::wstring operation = {}) {
  MfHardwareH264EncoderDrainResult result;
  result.status = status;
  result.message = std::move(message);
  result.hresult = hresult;
  result.device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device, hresult, std::move(operation));
  if (result.device_fault.Failed()) {
    result.message = result.device_fault.message;
  }
  return result;
}

void SetDrainFailure(MfHardwareH264EncoderDrainResult* result,
                     MfHardwareH264EncoderDrainStatus status,
                     std::wstring message,
                     HRESULT hresult,
                     ID3D11Device* d3d_device,
                     std::wstring operation) {
  result->status = status;
  result->message = std::move(message);
  result->hresult = hresult;
  result->device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device, hresult, std::move(operation));
  if (result->device_fault.Failed()) {
    result->message = result->device_fault.message;
  }
}

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

uint32_t EnumerationFlags(
    const MfHardwareH264EncoderProbeOptions& options) noexcept {
  uint32_t flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
  if (options.include_local_mfts) {
    flags |= MFT_ENUM_FLAG_LOCALMFT;
  }
  return flags;
}

HRESULT EnumerateActivates(const MfHardwareH264EncoderProbeOptions& options,
                           ActivateList* activates) {
  MFT_REGISTER_TYPE_INFO input_info;
  input_info.guidMajorType = MFMediaType_Video;
  input_info.guidSubtype = MFVideoFormat_NV12;

  MFT_REGISTER_TYPE_INFO output_info;
  output_info.guidMajorType = MFMediaType_Video;
  output_info.guidSubtype = MFVideoFormat_H264;

  return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, EnumerationFlags(options),
                   &input_info, &output_info, activates->put(),
                   activates->count_put());
}

uint32_t GopFrameCount(const MfHardwareH264EncoderConfig& config) noexcept {
  const double frames =
      config.gop_seconds *
      (static_cast<double>(config.fps_numerator) /
       static_cast<double>(config.fps_denominator));
  const double rounded = std::round(frames);
  if (rounded < 1.0) {
    return 1;
  }
  if (rounded > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(rounded);
}

LONGLONG NsToHns(int64_t ns) noexcept {
  return static_cast<LONGLONG>(ns / 100);
}

int64_t HnsToNs(LONGLONG hns) noexcept {
  return static_cast<int64_t>(hns) * 100;
}

size_t AnnexBStartCodeLength(const std::vector<uint8_t>& data,
                             size_t offset) noexcept {
  if (offset + 3u <= data.size() && data[offset] == 0 &&
      data[offset + 1u] == 0 && data[offset + 2u] == 1) {
    return 3u;
  }
  if (offset + 4u <= data.size() && data[offset] == 0 &&
      data[offset + 1u] == 0 && data[offset + 2u] == 0 &&
      data[offset + 3u] == 1) {
    return 4u;
  }
  return 0;
}

size_t FindNextAnnexBStartCode(const std::vector<uint8_t>& data,
                               size_t offset) noexcept {
  while (offset < data.size()) {
    if (AnnexBStartCodeLength(data, offset) != 0) {
      return offset;
    }
    ++offset;
  }
  return data.size();
}

HRESULT CreateSyntheticNv12Texture(
    ID3D11Device* d3d_device,
    const MfHardwareH264EncoderMediaTypePlan& plan,
    ID3D11Texture2D** texture) {
  if (d3d_device == nullptr || texture == nullptr || !plan.IsValid()) {
    return E_INVALIDARG;
  }

  const uint64_t y_size =
      static_cast<uint64_t>(plan.width) * static_cast<uint64_t>(plan.height);
  const uint64_t frame_size = y_size + (y_size / 2u);
  if (frame_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return E_OUTOFMEMORY;
  }

  std::vector<uint8_t> nv12(static_cast<size_t>(frame_size), 128);
  for (uint32_t y = 0; y < plan.height; ++y) {
    uint8_t* row = nv12.data() + (static_cast<size_t>(y) * plan.width);
    for (uint32_t x = 0; x < plan.width; ++x) {
      row[x] = static_cast<uint8_t>(16u + ((x + y) % 220u));
    }
  }

  uint8_t* uv_plane = nv12.data() + static_cast<size_t>(y_size);
  for (uint64_t index = 0; index < y_size / 2u; index += 2u) {
    uv_plane[index] = 128;
    uv_plane[index + 1u] = 128;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = plan.width;
  desc.Height = plan.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = nv12.data();
  initial_data.SysMemPitch = plan.width;
  initial_data.SysMemSlicePitch = static_cast<UINT>(frame_size);

  return d3d_device->CreateTexture2D(&desc, &initial_data, texture);
}

HRESULT CreateDxgiSampleFromTexture(
    ID3D11Texture2D* texture,
    const MfHardwareH264EncoderMediaTypePlan& plan,
    int64_t pts_ns,
    int64_t duration_ns,
    IMFSample** sample) {
  if (texture == nullptr || sample == nullptr || !plan.IsValid() ||
      pts_ns < 0 || duration_ns <= 0) {
    return E_INVALIDARG;
  }

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT result = MFCreateDXGISurfaceBuffer(
      __uuidof(ID3D11Texture2D), texture, 0, FALSE, buffer.put());
  if (FAILED(result)) {
    return result;
  }

  const uint64_t sample_size =
      (static_cast<uint64_t>(plan.width) * plan.height * 3u) / 2u;
  if (sample_size > std::numeric_limits<DWORD>::max()) {
    return E_INVALIDARG;
  }

  result = buffer.get()->SetCurrentLength(static_cast<DWORD>(sample_size));
  if (FAILED(result)) {
    return result;
  }

  ComPtr<IMFSample> created;
  result = MFCreateSample(created.put());
  if (FAILED(result)) {
    return result;
  }

  result = created.get()->AddBuffer(buffer.get());
  if (FAILED(result)) {
    return result;
  }

  result = created.get()->SetSampleTime(NsToHns(pts_ns));
  if (FAILED(result)) {
    return result;
  }

  result = created.get()->SetSampleDuration(NsToHns(duration_ns));
  if (FAILED(result)) {
    return result;
  }

  *sample = created.Detach();
  return S_OK;
}

bool TextureMatchesNv12Plan(
    ID3D11Texture2D* texture,
    const MfHardwareH264EncoderMediaTypePlan& plan) noexcept {
  if (texture == nullptr || !plan.IsValid()) {
    return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  return desc.Width == plan.width && desc.Height == plan.height &&
         desc.Format == DXGI_FORMAT_NV12 && desc.SampleDesc.Count == 1;
}

HRESULT CreateOutputSample(IMFTransform* transform, IMFSample** sample) {
  if (transform == nullptr || sample == nullptr) {
    return E_POINTER;
  }

  MFT_OUTPUT_STREAM_INFO stream_info{};
  HRESULT result = transform->GetOutputStreamInfo(0, &stream_info);
  if (FAILED(result)) {
    return result;
  }

  if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0) {
    *sample = nullptr;
    return S_OK;
  }

  ComPtr<IMFMediaBuffer> buffer;
  result = MFCreateMemoryBuffer(stream_info.cbSize, buffer.put());
  if (FAILED(result)) {
    return result;
  }

  ComPtr<IMFSample> created;
  result = MFCreateSample(created.put());
  if (FAILED(result)) {
    return result;
  }

  result = created.get()->AddBuffer(buffer.get());
  if (FAILED(result)) {
    return result;
  }

  *sample = created.Detach();
  return S_OK;
}

HRESULT ExtractPacketFromSample(IMFSample* sample,
                                MfHardwareH264EncodedPacket* packet) {
  if (sample == nullptr || packet == nullptr) {
    return E_POINTER;
  }

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT result = sample->ConvertToContiguousBuffer(buffer.put());
  if (FAILED(result)) {
    return result;
  }

  BYTE* data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  result = buffer.get()->Lock(&data, &max_length, &current_length);
  if (FAILED(result)) {
    return result;
  }

  packet->data.assign(data, data + current_length);
  buffer.get()->Unlock();

  LONGLONG value = 0;
  if (SUCCEEDED(sample->GetSampleTime(&value))) {
    packet->pts_ns = HnsToNs(value);
  }
  if (SUCCEEDED(sample->GetSampleDuration(&value))) {
    packet->duration_ns = HnsToNs(value);
  }

  UINT32 clean_point = 0;
  if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint,
                                  &clean_point))) {
    packet->keyframe = clean_point != 0;
  }
  packet->bitstream = InspectMfHardwareH264Bitstream(packet->data);

  return S_OK;
}

HRESULT CreateInputType(const MfHardwareH264EncoderMediaTypePlan& plan,
                        IMFMediaType** media_type) {
  ComPtr<IMFMediaType> type;
  HRESULT result = MFCreateMediaType(type.put());
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, plan.width,
                              plan.height);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE,
                               plan.fps_numerator, plan.fps_denominator);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_INTERLACE_MODE,
                                 MFVideoInterlace_Progressive);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (FAILED(result)) {
    return result;
  }

  *media_type = type.Detach();
  return S_OK;
}

HRESULT CreateOutputType(const MfHardwareH264EncoderMediaTypePlan& plan,
                         IMFMediaType** media_type) {
  ComPtr<IMFMediaType> type;
  HRESULT result = MFCreateMediaType(type.put());
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, plan.width,
                              plan.height);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE,
                               plan.fps_numerator, plan.fps_denominator);
  if (FAILED(result)) {
    return result;
  }

  result = MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_INTERLACE_MODE,
                                 MFVideoInterlace_Progressive);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AVG_BITRATE, plan.bitrate_bps);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_MPEG2_PROFILE, plan.h264_profile);
  if (FAILED(result)) {
    return result;
  }

  *media_type = type.Detach();
  return S_OK;
}

enum class CodecSettingValueKind {
  UnsignedInteger,
  Boolean,
};

bool ReadCodecSettingValue(const VARIANT& value,
                           uint32_t* accepted_value) noexcept {
  if (accepted_value == nullptr) {
    return false;
  }
  switch (value.vt) {
    case VT_UI4:
      *accepted_value = value.ulVal;
      return true;
    case VT_UINT:
      *accepted_value = value.uintVal;
      return true;
    case VT_I4:
      if (value.lVal >= 0) {
        *accepted_value = static_cast<uint32_t>(value.lVal);
        return true;
      }
      return false;
    case VT_INT:
      if (value.intVal >= 0) {
        *accepted_value = static_cast<uint32_t>(value.intVal);
        return true;
      }
      return false;
    case VT_BOOL:
      *accepted_value = value.boolVal == VARIANT_FALSE ? 0u : 1u;
      return true;
    default:
      return false;
  }
}

MfHardwareH264CodecSettingResult ApplyCodecSetting(
    ICodecAPI* codec_api,
    const GUID& key,
    std::wstring name,
    uint32_t value,
    CodecSettingValueKind value_kind = CodecSettingValueKind::UnsignedInteger,
    bool apply = true) {
  MfHardwareH264CodecSettingResult setting;
  setting.name = std::move(name);
  setting.requested_value = value;

  if (codec_api == nullptr) {
    setting.message = L"ICodecAPI is unavailable.";
    return setting;
  }

  setting.supported = codec_api->IsSupported(&key) == S_OK;
  setting.modifiable = codec_api->IsModifiable(&key) == S_OK;
  if (!apply) {
    setting.message = setting.supported
                          ? L"Supported; Balanced mode left it unchanged."
                          : L"Not reported as supported; Balanced mode left "
                            L"it unchanged.";
    return setting;
  }
  setting.attempted = true;

  VARIANT variant{};
  if (value_kind == CodecSettingValueKind::Boolean) {
    variant.vt = VT_BOOL;
    variant.boolVal = value == 0 ? VARIANT_FALSE : VARIANT_TRUE;
  } else {
    variant.vt = VT_UI4;
    variant.ulVal = value;
  }
  const HRESULT result = codec_api->SetValue(&key, &variant);
  setting.applied = SUCCEEDED(result);
  if (FAILED(result)) {
    setting.message = L"Codec setting rejected (" + HResultToHex(result) +
                      L").";
    return setting;
  }

  VARIANT accepted{};
  const HRESULT read_result = codec_api->GetValue(&key, &accepted);
  if (SUCCEEDED(read_result)) {
    setting.read_back =
        ReadCodecSettingValue(accepted, &setting.accepted_value);
  }
  VariantClear(&accepted);
  if (!setting.read_back) {
    setting.message = L"Applied; encoder readback was unavailable.";
  } else if (setting.accepted_value != setting.requested_value) {
    setting.message = L"Applied; encoder accepted value " +
                      std::to_wstring(setting.accepted_value) + L".";
  } else {
    setting.message = L"Applied and confirmed.";
  }
  return setting;
}

void ApplyCodecSettings(const MfHardwareH264EncoderMediaTypePlan& plan,
                        performance::CapturePerformanceMode performance_mode,
                        IMFTransform* transform,
                        MfHardwareH264EncoderSessionInfo* info) {
  ComPtr<ICodecAPI> codec_api;
  if (transform != nullptr) {
    (void)transform->QueryInterface(
        __uuidof(ICodecAPI),
        reinterpret_cast<void**>(codec_api.put()));
  }

  info->codec_api_available = codec_api.get() != nullptr;
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncCommonRateControlMode,
      L"rate control CBR", eAVEncCommonRateControlMode_CBR));
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncCommonMeanBitRate, L"mean bitrate",
      plan.bitrate_bps));
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncMPVGOPSize, L"GOP frame count",
      plan.gop_frame_count));
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncMPVDefaultBPictureCount,
      L"B-frame count", plan.max_b_frames));
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncH264CABACEnable, L"CABAC enabled", TRUE));

  const auto tuning =
      BuildMfHardwareH264PerformanceTuningPlan(performance_mode);
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVLowLatencyMode, L"low latency mode",
      tuning.requested_low_latency ? 1u : 0u,
      CodecSettingValueKind::Boolean, tuning.apply_low_latency));
  info->codec_settings.push_back(ApplyCodecSetting(
      codec_api.get(), CODECAPI_AVEncCommonQualityVsSpeed,
      L"quality versus speed", tuning.requested_quality_vs_speed,
      CodecSettingValueKind::UnsignedInteger,
      tuning.apply_quality_vs_speed));
}

HRESULT QueryTransformTraits(IMFTransform* transform,
                             MfHardwareH264EncoderSessionInfo* info) {
  ComPtr<IMFAttributes> attributes;
  HRESULT result = transform->GetAttributes(attributes.put());
  if (FAILED(result)) {
    return result;
  }

  UINT32 value = 0;
  if (SUCCEEDED(attributes.get()->GetUINT32(MF_SA_D3D11_AWARE, &value))) {
    info->d3d11_aware = value != 0;
  }

  value = 0;
  if (SUCCEEDED(attributes.get()->GetUINT32(MF_TRANSFORM_ASYNC, &value))) {
    info->async_transform = value != 0;
  }

  if (info->async_transform) {
    result = attributes.get()->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(result)) {
      return result;
    }
    info->async_unlocked = true;
  }

  return S_OK;
}

HRESULT AttachDxgiDeviceManager(ID3D11Device* d3d_device,
                                IMFTransform* transform,
                                IMFDXGIDeviceManager** device_manager,
                                uint32_t* reset_token,
                                MfHardwareH264EncoderSessionInfo* info) {
  if (info == nullptr || device_manager == nullptr || reset_token == nullptr) {
    return E_POINTER;
  }

  info->d3d11_device_supplied = d3d_device != nullptr;
  if (d3d_device == nullptr || !info->d3d11_aware) {
    return S_OK;
  }

  UINT token = 0;
  ComPtr<IMFDXGIDeviceManager> manager;
  HRESULT result = MFCreateDXGIDeviceManager(&token, manager.put());
  if (FAILED(result)) {
    return result;
  }
  info->device_manager_created = true;
  info->device_manager_reset_token = token;

  result = manager.get()->ResetDevice(d3d_device, token);
  if (FAILED(result)) {
    return result;
  }
  info->device_manager_reset = true;

  result = transform->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(manager.get()));
  if (FAILED(result)) {
    return result;
  }
  info->device_manager_attached = true;

  *reset_token = token;
  *device_manager = manager.Detach();
  return S_OK;
}

MfHardwareH264EncoderSessionStatus ProbeFailureStatus(
    MfHardwareH264EncoderProbeStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderProbeStatus::Success:
      return MfHardwareH264EncoderSessionStatus::Success;
    case MfHardwareH264EncoderProbeStatus::InvalidConfig:
      return MfHardwareH264EncoderSessionStatus::InvalidConfig;
    case MfHardwareH264EncoderProbeStatus::MediaFoundationUnavailable:
      return MfHardwareH264EncoderSessionStatus::MediaFoundationUnavailable;
    case MfHardwareH264EncoderProbeStatus::EnumerationFailed:
    case MfHardwareH264EncoderProbeStatus::HardwareEncoderUnavailable:
      return MfHardwareH264EncoderSessionStatus::ProbeFailed;
  }

  return MfHardwareH264EncoderSessionStatus::ProbeFailed;
}

}  // namespace

bool MfHardwareH264EncoderMediaTypePlan::IsValid() const noexcept {
  return width != 0 && height != 0 && fps_numerator != 0 &&
         fps_denominator != 0 && bitrate_bps != 0 && gop_frame_count != 0 &&
         h264_profile != 0 && max_b_frames == 0;
}

bool MfHardwareH264EncoderSessionResult::Succeeded() const noexcept {
  return status == MfHardwareH264EncoderSessionStatus::Success;
}

bool MfHardwareH264EncoderFrameSubmitResult::Succeeded() const noexcept {
  return status == MfHardwareH264EncoderFrameSubmitStatus::Success;
}

bool MfHardwareH264EncoderDrainResult::Succeeded() const noexcept {
  return status == MfHardwareH264EncoderDrainStatus::Success;
}

bool MfHardwareH264ConfigRecord::IsReady() const noexcept {
  return packet_format != MfHardwareH264PacketFormat::Unknown &&
         !sps.empty() && !pps.empty();
}

bool MfHardwareH264ConfigRecord::HasAvccExtradata() const noexcept {
  return !avcc_extradata.empty();
}

bool MfHardwareH264AvccExtradata::IsValid() const noexcept {
  return !bytes.empty();
}

MfHardwareH264EncoderSessionResult BuildMfHardwareH264EncoderMediaTypePlan(
    const MfHardwareH264EncoderConfig& config,
    MfHardwareH264EncoderMediaTypePlan* plan) {
  if (plan == nullptr) {
    return Result(MfHardwareH264EncoderSessionStatus::InvalidConfig,
                  L"H.264 encoder media type plan needs an output "
                  L"destination.");
  }
  *plan = {};

  const auto validation = ValidateMfHardwareH264EncoderConfig(config);
  if (!validation.Succeeded()) {
    auto result = Result(ProbeFailureStatus(validation.status),
                         validation.message);
    result.probe = validation;
    return result;
  }

  const double frames =
      config.gop_seconds *
      (static_cast<double>(config.fps_numerator) /
       static_cast<double>(config.fps_denominator));
  if (frames > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    return Result(MfHardwareH264EncoderSessionStatus::InvalidConfig,
                  L"H.264 encoder GOP frame count is too large.");
  }

  plan->width = config.width;
  plan->height = config.height;
  plan->fps_numerator = config.fps_numerator;
  plan->fps_denominator = config.fps_denominator;
  plan->bitrate_bps = config.bitrate_bps;
  plan->gop_frame_count = GopFrameCount(config);
  plan->h264_profile = eAVEncH264VProfile_High;
  plan->max_b_frames = config.max_b_frames;

  auto result = Result(MfHardwareH264EncoderSessionStatus::Success, L"");
  result.info.media_type = *plan;
  return result;
}

MfHardwareH264AvccExtradata BuildMfHardwareH264AvccExtradata(
    const MfHardwareH264ConfigRecord& config) {
  MfHardwareH264AvccExtradata extradata;
  if (!config.IsReady() || config.sps.size() < 4u ||
      config.sps.size() > std::numeric_limits<uint16_t>::max() ||
      config.pps.size() > std::numeric_limits<uint16_t>::max()) {
    return extradata;
  }

  const auto append_u16_be = [&extradata](uint16_t value) {
    extradata.bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    extradata.bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
  };

  extradata.bytes.reserve(11u + config.sps.size() + config.pps.size());
  extradata.bytes.push_back(1u);
  extradata.bytes.push_back(config.sps[1]);
  extradata.bytes.push_back(config.sps[2]);
  extradata.bytes.push_back(config.sps[3]);
  extradata.bytes.push_back(0xFFu);
  extradata.bytes.push_back(0xE1u);
  append_u16_be(static_cast<uint16_t>(config.sps.size()));
  extradata.bytes.insert(extradata.bytes.end(), config.sps.begin(),
                         config.sps.end());
  extradata.bytes.push_back(1u);
  append_u16_be(static_cast<uint16_t>(config.pps.size()));
  extradata.bytes.insert(extradata.bytes.end(), config.pps.begin(),
                         config.pps.end());
  return extradata;
}

MfHardwareH264BitstreamInfo InspectMfHardwareH264Bitstream(
    const std::vector<uint8_t>& data) {
  MfHardwareH264BitstreamInfo info;
  size_t offset = 0;
  while (offset < data.size()) {
    const size_t start_code_length = AnnexBStartCodeLength(data, offset);
    if (start_code_length == 0) {
      ++offset;
      continue;
    }

    const size_t nal_header_offset = offset + start_code_length;
    if (nal_header_offset >= data.size()) {
      break;
    }

    info.packet_format = MfHardwareH264PacketFormat::AnnexB;
    info.config.packet_format = MfHardwareH264PacketFormat::AnnexB;
    ++info.nal_unit_count;

    const size_t next_start_code =
        FindNextAnnexBStartCode(data, nal_header_offset + 1u);
    const uint8_t nal_type = data[nal_header_offset] & 0x1Fu;
    if (nal_type == 7u) {
      ++info.sps_count;
      info.has_sps = true;
      if (info.config.sps.empty()) {
        info.config.sps.assign(data.begin() + nal_header_offset,
                               data.begin() + next_start_code);
      }
    } else if (nal_type == 8u) {
      ++info.pps_count;
      info.has_pps = true;
      if (info.config.pps.empty()) {
        info.config.pps.assign(data.begin() + nal_header_offset,
                               data.begin() + next_start_code);
      }
    } else if (nal_type == 5u) {
      ++info.idr_count;
      info.has_idr = true;
    }

    offset = next_start_code;
  }

  auto avcc = BuildMfHardwareH264AvccExtradata(info.config);
  info.config.avcc_extradata = std::move(avcc.bytes);
  info.mp4_extradata_ready = info.config.HasAvccExtradata();
  return info;
}

MfHardwareH264EncoderSession::~MfHardwareH264EncoderSession() {
  Reset();
}

MfHardwareH264EncoderSessionResult MfHardwareH264EncoderSession::Initialize(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options) {
  return Initialize(config, options, nullptr);
}

MfHardwareH264EncoderSessionResult MfHardwareH264EncoderSession::Initialize(
    const MfHardwareH264EncoderConfig& config,
    const MfHardwareH264EncoderProbeOptions& options,
    ID3D11Device* d3d_device) {
  Reset();

  MfHardwareH264EncoderMediaTypePlan media_type_plan;
  auto plan_result =
      BuildMfHardwareH264EncoderMediaTypePlan(config, &media_type_plan);
  if (!plan_result.Succeeded()) {
    return plan_result;
  }

  auto probe = ProbeMfHardwareH264Encoder(config, options);
  if (!probe.Succeeded()) {
    auto result =
        Result(ProbeFailureStatus(probe.status), probe.message);
    result.probe = std::move(probe);
    return result;
  }

  HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::MediaFoundationUnavailable,
               L"Could not start Media Foundation (" + HResultToHex(result) +
                   L").");
    session_result.probe = std::move(probe);
    return session_result;
  }
  mf_started_ = true;

  ActivateList activates;
  result = EnumerateActivates(options, &activates);
  if (FAILED(result) || activates.count() == 0) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::ActivationFailed,
               L"Could not re-enumerate the selected hardware H.264 encoder "
               L"MFT (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    Reset();
    return session_result;
  }

  ComPtr<IMFTransform> transform;
  result = activates[0]->ActivateObject(
      __uuidof(IMFTransform), reinterpret_cast<void**>(transform.put()));
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::ActivationFailed,
               L"Could not activate hardware H.264 encoder MFT (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    Reset();
    return session_result;
  }

  info_ = {};
  info_.encoder = *probe.selected_encoder();
  info_.media_type = media_type_plan;

  result = QueryTransformTraits(transform.get(), &info_);
  if (FAILED(result)) {
    auto session_result =
        Result(info_.async_transform
                   ? MfHardwareH264EncoderSessionStatus::AsyncUnlockFailed
                   : MfHardwareH264EncoderSessionStatus::AttributeQueryFailed,
               L"Could not query or unlock H.264 transform attributes (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }

  IMFDXGIDeviceManager* attached_manager = nullptr;
  uint32_t reset_token = 0;
  result = AttachDxgiDeviceManager(d3d_device, transform.get(),
                                   &attached_manager, &reset_token, &info_);
  if (FAILED(result)) {
    auto session_result =
        Result(!info_.device_manager_created
                   ? MfHardwareH264EncoderSessionStatus::
                         DeviceManagerCreateFailed
               : !info_.device_manager_reset
                   ? MfHardwareH264EncoderSessionStatus::
                         DeviceManagerResetFailed
                   : MfHardwareH264EncoderSessionStatus::
                         DeviceManagerAttachFailed,
               L"Could not attach DXGI device manager to H.264 transform (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }
  device_manager_ = attached_manager;
  device_manager_reset_token_ = reset_token;

  ApplyCodecSettings(media_type_plan, config.performance_mode,
                     transform.get(), &info_);

  ComPtr<IMFMediaType> output_type;
  result = CreateOutputType(media_type_plan, output_type.put());
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::MediaTypeCreateFailed,
               L"Could not create H.264 output media type (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }

  result = transform.get()->SetOutputType(0, output_type.get(), 0);
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::OutputTypeRejected,
               L"Hardware H.264 encoder rejected output media type (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }
  info_.output_type_configured = true;

  ComPtr<IMFMediaType> input_type;
  result = CreateInputType(media_type_plan, input_type.put());
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::MediaTypeCreateFailed,
               L"Could not create NV12 input media type (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }

  result = transform.get()->SetInputType(0, input_type.get(), 0);
  if (FAILED(result)) {
    auto session_result =
        Result(MfHardwareH264EncoderSessionStatus::InputTypeRejected,
               L"Hardware H.264 encoder rejected NV12 input media type (" +
                   HResultToHex(result) + L").");
    session_result.probe = std::move(probe);
    session_result.info = info_;
    Reset();
    return session_result;
  }
  info_.input_type_configured = true;

  if (d3d_device != nullptr) {
    d3d_device->AddRef();
    d3d_device_ = d3d_device;
  }
  transform_ = transform.Detach();
  configured_ = true;

  auto session_result =
      Result(MfHardwareH264EncoderSessionStatus::Success, L"");
  session_result.probe = std::move(probe);
  session_result.info = info_;
  return session_result;
}

MfHardwareH264EncoderFrameSubmitResult
MfHardwareH264EncoderSession::SubmitSyntheticNv12Frame(ID3D11Device* d3d_device,
                                                       int64_t pts_ns,
                                                       int64_t duration_ns) {
  if (!IsConfigured()) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::NotConfigured,
        L"H.264 encoder session is not configured.");
  }
  if (d3d_device == nullptr || pts_ns < 0 || duration_ns <= 0) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument,
        L"Synthetic NV12 frame submission needs a D3D11 device and "
        L"non-negative timing.");
  }
  if (drain_command_sent_) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument,
        L"Synthetic H.264 input cannot be submitted after drain starts.");
  }

  ComPtr<ID3D11Texture2D> texture;
  HRESULT result =
      CreateSyntheticNv12Texture(d3d_device, info_.media_type, texture.put());
  if (FAILED(result)) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::TextureCreateFailed,
        L"Could not create synthetic NV12 D3D11 texture (" +
            HResultToHex(result) + L").",
        result, d3d_device, L"synthetic NV12 texture creation");
  }

  ComPtr<IMFSample> sample;
  result = CreateDxgiSampleFromTexture(texture.get(), info_.media_type, pts_ns,
                                       duration_ns, sample.put());
  if (FAILED(result)) {
    auto submit_result = FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::SampleCreateFailed,
        L"Could not wrap synthetic NV12 texture in an MF sample (" +
            HResultToHex(result) + L").",
        result, d3d_device, L"synthetic MF DXGI sample creation");
    submit_result.synthetic_texture_created = true;
    return submit_result;
  }

  if (!stream_started_) {
    result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(result)) {
      result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,
                                          0);
    }
    if (FAILED(result)) {
      auto submit_result = FrameSubmitResult(
          MfHardwareH264EncoderFrameSubmitStatus::StreamStartFailed,
          L"Could not start H.264 transform input stream (" +
              HResultToHex(result) + L").",
          result, d3d_device, L"hardware H.264 stream start");
      submit_result.synthetic_texture_created = true;
      submit_result.sample_created = true;
      return submit_result;
    }
    stream_started_ = true;
  }

  result = transform_->ProcessInput(0, sample.get(), 0);
  if (FAILED(result)) {
    auto submit_result = FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed,
        L"Hardware H.264 encoder rejected synthetic NV12 input sample (" +
            HResultToHex(result) + L").",
        result, d3d_device, L"hardware H.264 ProcessInput");
    submit_result.synthetic_texture_created = true;
    submit_result.sample_created = true;
    submit_result.stream_started = true;
    return submit_result;
  }

  pending_input_samples_.push_back(sample.Detach());
  ++submitted_input_frames_;

  info_.synthetic_input_submitted = true;
  info_.submitted_input_frames = submitted_input_frames_;
  info_.pending_input_samples = pending_input_samples_.size();

  auto device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device, S_OK, L"hardware H.264 synthetic input submission");
  if (device_fault.Failed()) {
    auto failed = FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed,
        device_fault.message, S_OK, d3d_device,
        L"hardware H.264 synthetic input submission");
    failed.synthetic_texture_created = true;
    failed.sample_created = true;
    failed.stream_started = true;
    failed.input_submitted = true;
    failed.submitted_input_frames = submitted_input_frames_;
    return failed;
  }

  auto submit_result =
      FrameSubmitResult(MfHardwareH264EncoderFrameSubmitStatus::Success, L"");
  submit_result.synthetic_texture_created = true;
  submit_result.sample_created = true;
  submit_result.stream_started = true;
  submit_result.input_submitted = true;
  submit_result.submitted_input_frames = submitted_input_frames_;
  return submit_result;
}

MfHardwareH264EncoderFrameSubmitResult
MfHardwareH264EncoderSession::SubmitNv12Texture(ID3D11Texture2D* nv12_texture,
                                                int64_t pts_ns,
                                                int64_t duration_ns) {
  if (!IsConfigured()) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::NotConfigured,
        L"H.264 encoder session is not configured.");
  }
  if (nv12_texture == nullptr || pts_ns < 0 || duration_ns <= 0) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument,
        L"Caller-owned NV12 frame submission needs a texture and "
        L"non-negative timing.");
  }
  if (drain_command_sent_) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument,
        L"Caller-owned H.264 input cannot be submitted after drain starts.");
  }
  if (!TextureMatchesNv12Plan(nv12_texture, info_.media_type)) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument,
        L"Caller-owned NV12 texture does not match the configured H.264 "
        L"input media type.");
  }

  ComPtr<IMFSample> sample;
  HRESULT result = CreateDxgiSampleFromTexture(
      nv12_texture, info_.media_type, pts_ns, duration_ns, sample.put());
  if (FAILED(result)) {
    return FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::SampleCreateFailed,
        L"Could not wrap caller-owned NV12 texture in an MF sample (" +
            HResultToHex(result) + L").",
        result, d3d_device_, L"encoder MF DXGI sample creation");
  }

  if (!stream_started_) {
    result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(result)) {
      result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,
                                          0);
    }
    if (FAILED(result)) {
      auto submit_result = FrameSubmitResult(
          MfHardwareH264EncoderFrameSubmitStatus::StreamStartFailed,
          L"Could not start H.264 transform input stream (" +
              HResultToHex(result) + L").",
          result, d3d_device_, L"hardware H.264 stream start");
      submit_result.sample_created = true;
      return submit_result;
    }
    stream_started_ = true;
  }

  result = transform_->ProcessInput(0, sample.get(), 0);
  if (FAILED(result)) {
    auto submit_result = FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed,
        L"Hardware H.264 encoder rejected caller-owned NV12 input sample (" +
            HResultToHex(result) + L").",
        result, d3d_device_, L"hardware H.264 ProcessInput");
    submit_result.sample_created = true;
    submit_result.stream_started = true;
    return submit_result;
  }

  pending_input_samples_.push_back(sample.Detach());
  ++submitted_input_frames_;

  info_.submitted_input_frames = submitted_input_frames_;
  info_.pending_input_samples = pending_input_samples_.size();

  auto device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device_, S_OK, L"hardware H.264 input submission");
  if (device_fault.Failed()) {
    auto failed = FrameSubmitResult(
        MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed,
        device_fault.message, S_OK, d3d_device_,
        L"hardware H.264 input submission");
    failed.sample_created = true;
    failed.stream_started = true;
    failed.input_submitted = true;
    failed.submitted_input_frames = submitted_input_frames_;
    return failed;
  }

  auto submit_result =
      FrameSubmitResult(MfHardwareH264EncoderFrameSubmitStatus::Success, L"");
  submit_result.sample_created = true;
  submit_result.stream_started = true;
  submit_result.input_submitted = true;
  submit_result.submitted_input_frames = submitted_input_frames_;
  return submit_result;
}

MfHardwareH264EncoderDrainResult
MfHardwareH264EncoderSession::DrainSyntheticAvailableOutput(
    uint32_t timeout_ms) {
  return DrainSyntheticOutput(timeout_ms, false);
}

MfHardwareH264EncoderDrainResult
MfHardwareH264EncoderSession::DrainSyntheticEncodedOutput(
    uint32_t timeout_ms) {
  return DrainSyntheticOutput(timeout_ms, true);
}

MfHardwareH264EncoderDrainResult
MfHardwareH264EncoderSession::DrainSyntheticOutput(uint32_t timeout_ms,
                                                   bool request_drain) {
  if (!IsConfigured()) {
    return DrainResult(MfHardwareH264EncoderDrainStatus::NotConfigured,
                       L"H.264 encoder session is not configured.");
  }
  if (submitted_input_frames_ == 0) {
    return DrainResult(MfHardwareH264EncoderDrainStatus::NoInputSubmitted,
                       L"No synthetic H.264 input frame has been submitted.");
  }

  ComPtr<IMFMediaEventGenerator> events;
  HRESULT result =
      transform_->QueryInterface(__uuidof(IMFMediaEventGenerator),
                                 reinterpret_cast<void**>(events.put()));
  if (FAILED(result)) {
    return DrainResult(
        MfHardwareH264EncoderDrainStatus::EventInterfaceUnavailable,
        L"H.264 transform does not expose IMFMediaEventGenerator (" +
            HResultToHex(result) + L").",
        result, d3d_device_, L"hardware H.264 event-interface query");
  }

  auto drain_result =
      DrainResult(MfHardwareH264EncoderDrainStatus::TimedOut,
                  L"No encoded H.264 output was produced before timeout.",
                  S_OK, d3d_device_, L"hardware H.264 output wait");
  const uint64_t target_output_packets =
      submitted_input_frames_ > drained_output_packets_
          ? submitted_input_frames_ - drained_output_packets_
          : 0;

  if (request_drain && !drain_command_sent_) {
    result = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(result)) {
      return DrainResult(MfHardwareH264EncoderDrainStatus::DrainCommandFailed,
                         L"Could not request H.264 transform drain (" +
                             HResultToHex(result) + L").",
                         result, d3d_device_,
                         L"hardware H.264 drain command");
    }
    drain_command_sent_ = true;
  }
  drain_result.drain_command_sent = drain_command_sent_;

  const DWORD start_tick = GetTickCount();
  const DWORD timeout = timeout_ms == 0 ? 1 : timeout_ms;

  while (GetTickCount() - start_tick <= timeout) {
    ComPtr<IMFMediaEvent> event;
    result = events.get()->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.put());
    if (result == MF_E_NO_EVENTS_AVAILABLE) {
      Sleep(1);
      continue;
    }
    if (FAILED(result)) {
      SetDrainFailure(
          &drain_result,
          MfHardwareH264EncoderDrainStatus::EventWaitFailed,
          L"Could not read H.264 transform event (" + HResultToHex(result) +
              L").",
          result, d3d_device_, L"hardware H.264 event wait");
      return drain_result;
    }

    ++drain_result.events_checked;

    MediaEventType event_type = MEUnknown;
    result = event.get()->GetType(&event_type);
    if (FAILED(result)) {
      SetDrainFailure(
          &drain_result,
          MfHardwareH264EncoderDrainStatus::EventWaitFailed,
          L"Could not read H.264 transform event type (" +
              HResultToHex(result) + L").",
          result, d3d_device_, L"hardware H.264 event-type query");
      return drain_result;
    }

    if (event_type == METransformNeedInput) {
      drain_result.saw_need_input_event = true;
      continue;
    }
    if (request_drain && event_type == METransformDrainComplete) {
      ReleaseAllPendingInputSamples();
      drain_result.status = MfHardwareH264EncoderDrainStatus::Success;
      drain_result.message.clear();
      return drain_result;
    }
    if (event_type != METransformHaveOutput) {
      continue;
    }

    drain_result.saw_have_output_event = true;

    ComPtr<IMFSample> output_sample;
    result = CreateOutputSample(transform_, output_sample.put());
    if (FAILED(result)) {
      SetDrainFailure(
          &drain_result,
          MfHardwareH264EncoderDrainStatus::OutputSampleCreateFailed,
          L"Could not create H.264 output sample (" + HResultToHex(result) +
              L").",
          result, d3d_device_, L"hardware H.264 output-sample creation");
      return drain_result;
    }

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = output_sample.get();

    DWORD output_status = 0;
    result = transform_->ProcessOutput(0, 1, &output, &output_status);

    if (FAILED(result)) {
      if (output.pEvents != nullptr) {
        output.pEvents->Release();
      }
      if (output.pSample != nullptr && output.pSample != output_sample.get()) {
        output.pSample->Release();
      }
      SetDrainFailure(
          &drain_result,
          MfHardwareH264EncoderDrainStatus::ProcessOutputFailed,
          L"Could not drain H.264 encoded output (" + HResultToHex(result) +
              L").",
          result, d3d_device_, L"hardware H.264 ProcessOutput");
      return drain_result;
    }

    IMFSample* sample_to_read =
        output.pSample != nullptr ? output.pSample : output_sample.get();
    MfHardwareH264EncodedPacket packet;
    result = ExtractPacketFromSample(sample_to_read, &packet);

    if (output.pEvents != nullptr) {
      output.pEvents->Release();
    }
    if (output.pSample != nullptr && output.pSample != output_sample.get()) {
      output.pSample->Release();
    }

    if (FAILED(result)) {
      SetDrainFailure(
          &drain_result,
          MfHardwareH264EncoderDrainStatus::SampleReadFailed,
          L"Could not read H.264 output sample (" + HResultToHex(result) +
              L").",
          result, d3d_device_, L"hardware H.264 output-sample read");
      return drain_result;
    }

    if (!packet.data.empty()) {
      ReleaseOldestPendingInputSample();
      drain_result.packets.push_back(std::move(packet));
      ++drained_output_packets_;
      drained_output_bytes_ += drain_result.packets.back().data.size();
      info_.drained_output_packets = drained_output_packets_;
      info_.drained_output_bytes = drained_output_bytes_;
      info_.bitstream = drain_result.packets.back().bitstream;
      drain_result.status = MfHardwareH264EncoderDrainStatus::Success;
      drain_result.message.clear();
      if (target_output_packets == 0 ||
          drain_result.packets.size() >= target_output_packets) {
        return drain_result;
      }
    }
  }

  if (!drain_result.packets.empty()) {
    drain_result.status = MfHardwareH264EncoderDrainStatus::Success;
    drain_result.message.clear();
  }
  auto device_fault = graphics::InspectD3D11DeviceFault(
      d3d_device_, S_OK, L"hardware H.264 output drain");
  if (device_fault.Failed()) {
    SetDrainFailure(
        &drain_result,
        MfHardwareH264EncoderDrainStatus::ProcessOutputFailed,
        device_fault.message, S_OK, d3d_device_,
        L"hardware H.264 output drain");
  }
  return drain_result;
}

void MfHardwareH264EncoderSession::Reset() noexcept {
  if (transform_ != nullptr) {
    transform_->Release();
    transform_ = nullptr;
  }
  if (device_manager_ != nullptr) {
    device_manager_->Release();
    device_manager_ = nullptr;
  }
  if (d3d_device_ != nullptr) {
    d3d_device_->Release();
    d3d_device_ = nullptr;
  }
  ReleaseAllPendingInputSamples();
  device_manager_reset_token_ = 0;
  submitted_input_frames_ = 0;
  drained_output_packets_ = 0;
  drained_output_bytes_ = 0;
  stream_started_ = false;
  drain_command_sent_ = false;
  configured_ = false;
  info_ = {};

  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
}

bool MfHardwareH264EncoderSession::IsConfigured() const noexcept {
  return configured_ && transform_ != nullptr;
}

IMFTransform* MfHardwareH264EncoderSession::transform() const noexcept {
  return transform_;
}

const MfHardwareH264EncoderSessionInfo& MfHardwareH264EncoderSession::info()
    const noexcept {
  return info_;
}

size_t MfHardwareH264EncoderSession::pending_input_sample_count()
    const noexcept {
  return pending_input_samples_.size();
}

void MfHardwareH264EncoderSession::ReleaseOldestPendingInputSample() noexcept {
  if (pending_input_samples_.empty()) {
    info_.pending_input_samples = 0;
    return;
  }
  IMFSample* sample = pending_input_samples_.front();
  pending_input_samples_.pop_front();
  if (sample != nullptr) {
    sample->Release();
  }
  info_.pending_input_samples = pending_input_samples_.size();
}

void MfHardwareH264EncoderSession::ReleaseAllPendingInputSamples() noexcept {
  for (IMFSample* sample : pending_input_samples_) {
    if (sample != nullptr) {
      sample->Release();
    }
  }
  pending_input_samples_.clear();
  info_.pending_input_samples = 0;
}

const wchar_t* MfHardwareH264EncoderSessionStatusName(
    MfHardwareH264EncoderSessionStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderSessionStatus::Success:
      return L"success";
    case MfHardwareH264EncoderSessionStatus::InvalidConfig:
      return L"invalid config";
    case MfHardwareH264EncoderSessionStatus::ProbeFailed:
      return L"probe failed";
    case MfHardwareH264EncoderSessionStatus::MediaFoundationUnavailable:
      return L"media foundation unavailable";
    case MfHardwareH264EncoderSessionStatus::ActivationFailed:
      return L"activation failed";
    case MfHardwareH264EncoderSessionStatus::AttributeQueryFailed:
      return L"attribute query failed";
    case MfHardwareH264EncoderSessionStatus::AsyncUnlockFailed:
      return L"async unlock failed";
    case MfHardwareH264EncoderSessionStatus::DeviceManagerCreateFailed:
      return L"device manager create failed";
    case MfHardwareH264EncoderSessionStatus::DeviceManagerResetFailed:
      return L"device manager reset failed";
    case MfHardwareH264EncoderSessionStatus::DeviceManagerAttachFailed:
      return L"device manager attach failed";
    case MfHardwareH264EncoderSessionStatus::MediaTypeCreateFailed:
      return L"media type create failed";
    case MfHardwareH264EncoderSessionStatus::OutputTypeRejected:
      return L"output type rejected";
    case MfHardwareH264EncoderSessionStatus::InputTypeRejected:
      return L"input type rejected";
  }

  return L"unknown";
}

const wchar_t* MfHardwareH264EncoderFrameSubmitStatusName(
    MfHardwareH264EncoderFrameSubmitStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderFrameSubmitStatus::Success:
      return L"success";
    case MfHardwareH264EncoderFrameSubmitStatus::NotConfigured:
      return L"not configured";
    case MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument:
      return L"invalid argument";
    case MfHardwareH264EncoderFrameSubmitStatus::TextureCreateFailed:
      return L"texture create failed";
    case MfHardwareH264EncoderFrameSubmitStatus::SampleCreateFailed:
      return L"sample create failed";
    case MfHardwareH264EncoderFrameSubmitStatus::StreamStartFailed:
      return L"stream start failed";
    case MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed:
      return L"process input failed";
  }

  return L"unknown";
}

const wchar_t* MfHardwareH264EncoderDrainStatusName(
    MfHardwareH264EncoderDrainStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderDrainStatus::Success:
      return L"success";
    case MfHardwareH264EncoderDrainStatus::NotConfigured:
      return L"not configured";
    case MfHardwareH264EncoderDrainStatus::NoInputSubmitted:
      return L"no input submitted";
    case MfHardwareH264EncoderDrainStatus::EventInterfaceUnavailable:
      return L"event interface unavailable";
    case MfHardwareH264EncoderDrainStatus::DrainCommandFailed:
      return L"drain command failed";
    case MfHardwareH264EncoderDrainStatus::EventWaitFailed:
      return L"event wait failed";
    case MfHardwareH264EncoderDrainStatus::OutputStreamInfoFailed:
      return L"output stream info failed";
    case MfHardwareH264EncoderDrainStatus::OutputSampleCreateFailed:
      return L"output sample create failed";
    case MfHardwareH264EncoderDrainStatus::ProcessOutputFailed:
      return L"process output failed";
    case MfHardwareH264EncoderDrainStatus::SampleReadFailed:
      return L"sample read failed";
    case MfHardwareH264EncoderDrainStatus::TimedOut:
      return L"timed out";
  }

  return L"unknown";
}

const wchar_t* MfHardwareH264PacketFormatName(
    MfHardwareH264PacketFormat format) noexcept {
  switch (format) {
    case MfHardwareH264PacketFormat::Unknown:
      return L"unknown";
    case MfHardwareH264PacketFormat::AnnexB:
      return L"annex b";
  }

  return L"unknown";
}

bool IsMfHardwareH264EncoderRuntimeFailure(
    MfHardwareH264EncoderFrameSubmitStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderFrameSubmitStatus::SampleCreateFailed:
    case MfHardwareH264EncoderFrameSubmitStatus::StreamStartFailed:
    case MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed:
      return true;
    case MfHardwareH264EncoderFrameSubmitStatus::Success:
    case MfHardwareH264EncoderFrameSubmitStatus::NotConfigured:
    case MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument:
    case MfHardwareH264EncoderFrameSubmitStatus::TextureCreateFailed:
      return false;
  }
  return false;
}

bool IsMfHardwareH264EncoderRuntimeFailure(
    MfHardwareH264EncoderDrainStatus status) noexcept {
  switch (status) {
    case MfHardwareH264EncoderDrainStatus::EventInterfaceUnavailable:
    case MfHardwareH264EncoderDrainStatus::DrainCommandFailed:
    case MfHardwareH264EncoderDrainStatus::EventWaitFailed:
    case MfHardwareH264EncoderDrainStatus::OutputStreamInfoFailed:
    case MfHardwareH264EncoderDrainStatus::OutputSampleCreateFailed:
    case MfHardwareH264EncoderDrainStatus::ProcessOutputFailed:
    case MfHardwareH264EncoderDrainStatus::SampleReadFailed:
    case MfHardwareH264EncoderDrainStatus::TimedOut:
      return true;
    case MfHardwareH264EncoderDrainStatus::Success:
    case MfHardwareH264EncoderDrainStatus::NotConfigured:
    case MfHardwareH264EncoderDrainStatus::NoInputSubmitted:
      return false;
  }
  return false;
}

}  // namespace olouie::encode
