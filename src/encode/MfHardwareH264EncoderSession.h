#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "encode/MfHardwareH264EncoderProbe.h"
#include "graphics/D3D11DeviceFault.h"

struct IMFTransform;
struct IMFDXGIDeviceManager;
struct IMFSample;
struct ID3D11Device;
struct ID3D11Texture2D;

namespace olouie::encode {

enum class MfHardwareH264EncoderSessionStatus {
  Success,
  InvalidConfig,
  ProbeFailed,
  MediaFoundationUnavailable,
  ActivationFailed,
  AttributeQueryFailed,
  AsyncUnlockFailed,
  DeviceManagerCreateFailed,
  DeviceManagerResetFailed,
  DeviceManagerAttachFailed,
  MediaTypeCreateFailed,
  OutputTypeRejected,
  InputTypeRejected,
};

enum class MfHardwareH264EncoderFrameSubmitStatus {
  Success,
  NotConfigured,
  InvalidArgument,
  TextureCreateFailed,
  SampleCreateFailed,
  StreamStartFailed,
  ProcessInputFailed,
};

enum class MfHardwareH264EncoderDrainStatus {
  Success,
  NotConfigured,
  NoInputSubmitted,
  EventInterfaceUnavailable,
  DrainCommandFailed,
  EventWaitFailed,
  OutputStreamInfoFailed,
  OutputSampleCreateFailed,
  ProcessOutputFailed,
  SampleReadFailed,
  TimedOut,
};

struct MfHardwareH264EncoderMediaTypePlan {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_numerator = 0;
  uint32_t fps_denominator = 0;
  uint32_t bitrate_bps = 0;
  uint32_t gop_frame_count = 0;
  uint32_t h264_profile = 0;
  uint32_t max_b_frames = 0;

  bool IsValid() const noexcept;
};

struct MfHardwareH264CodecSettingResult {
  std::wstring name;
  uint32_t requested_value = 0;
  bool supported = false;
  bool modifiable = false;
  bool attempted = false;
  bool applied = false;
  bool read_back = false;
  uint32_t accepted_value = 0;
  std::wstring message;
};

enum class MfHardwareH264PacketFormat {
  Unknown,
  AnnexB,
};

struct MfHardwareH264ConfigRecord {
  MfHardwareH264PacketFormat packet_format = MfHardwareH264PacketFormat::Unknown;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  std::vector<uint8_t> avcc_extradata;

  bool IsReady() const noexcept;
  bool HasAvccExtradata() const noexcept;
};

struct MfHardwareH264AvccExtradata {
  std::vector<uint8_t> bytes;

  bool IsValid() const noexcept;
};

struct MfHardwareH264BitstreamInfo {
  MfHardwareH264PacketFormat packet_format = MfHardwareH264PacketFormat::Unknown;
  uint32_t nal_unit_count = 0;
  uint32_t sps_count = 0;
  uint32_t pps_count = 0;
  uint32_t idr_count = 0;
  bool has_sps = false;
  bool has_pps = false;
  bool has_idr = false;
  bool mp4_extradata_ready = false;
  MfHardwareH264ConfigRecord config;
};

struct MfHardwareH264EncoderSessionInfo {
  MfHardwareH264EncoderInfo encoder;
  MfHardwareH264EncoderMediaTypePlan media_type;
  bool d3d11_aware = false;
  bool d3d11_device_supplied = false;
  bool device_manager_created = false;
  bool device_manager_reset = false;
  bool device_manager_attached = false;
  uint32_t device_manager_reset_token = 0;
  bool async_transform = false;
  bool async_unlocked = false;
  bool codec_api_available = false;
  bool output_type_configured = false;
  bool input_type_configured = false;
  bool synthetic_input_submitted = false;
  uint64_t submitted_input_frames = 0;
  size_t pending_input_samples = 0;
  uint64_t drained_output_packets = 0;
  uint64_t drained_output_bytes = 0;
  MfHardwareH264BitstreamInfo bitstream;
  std::vector<MfHardwareH264CodecSettingResult> codec_settings;
};

struct MfHardwareH264EncoderSessionResult {
  MfHardwareH264EncoderSessionStatus status =
      MfHardwareH264EncoderSessionStatus::InvalidConfig;
  std::wstring message;
  MfHardwareH264EncoderProbeResult probe;
  MfHardwareH264EncoderSessionInfo info;

  bool Succeeded() const noexcept;
};

struct MfHardwareH264EncoderFrameSubmitResult {
  MfHardwareH264EncoderFrameSubmitStatus status =
      MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument;
  std::wstring message;
  bool synthetic_texture_created = false;
  bool sample_created = false;
  bool stream_started = false;
  bool input_submitted = false;
  uint64_t submitted_input_frames = 0;
  HRESULT hresult = S_OK;
  graphics::D3D11DeviceFault device_fault;

  bool Succeeded() const noexcept;
};

struct MfHardwareH264EncodedPacket {
  int64_t pts_ns = 0;
  int64_t duration_ns = 0;
  bool keyframe = false;
  MfHardwareH264BitstreamInfo bitstream;
  std::vector<uint8_t> data;
};

struct MfHardwareH264EncoderDrainResult {
  MfHardwareH264EncoderDrainStatus status =
      MfHardwareH264EncoderDrainStatus::NotConfigured;
  std::wstring message;
  std::vector<MfHardwareH264EncodedPacket> packets;
  uint64_t events_checked = 0;
  bool drain_command_sent = false;
  bool saw_have_output_event = false;
  bool saw_need_input_event = false;
  HRESULT hresult = S_OK;
  graphics::D3D11DeviceFault device_fault;

  bool Succeeded() const noexcept;
};

MfHardwareH264EncoderSessionResult BuildMfHardwareH264EncoderMediaTypePlan(
    const MfHardwareH264EncoderConfig& config,
    MfHardwareH264EncoderMediaTypePlan* plan);
MfHardwareH264AvccExtradata BuildMfHardwareH264AvccExtradata(
    const MfHardwareH264ConfigRecord& config);
MfHardwareH264BitstreamInfo InspectMfHardwareH264Bitstream(
    const std::vector<uint8_t>& data);

class MfHardwareH264EncoderSession final {
 public:
  MfHardwareH264EncoderSession() = default;
  ~MfHardwareH264EncoderSession();

  MfHardwareH264EncoderSession(const MfHardwareH264EncoderSession&) = delete;
  MfHardwareH264EncoderSession& operator=(
      const MfHardwareH264EncoderSession&) = delete;

  MfHardwareH264EncoderSessionResult Initialize(
      const MfHardwareH264EncoderConfig& config,
      const MfHardwareH264EncoderProbeOptions& options);
  MfHardwareH264EncoderSessionResult Initialize(
      const MfHardwareH264EncoderConfig& config,
      const MfHardwareH264EncoderProbeOptions& options,
      ID3D11Device* d3d_device);
  void Reset() noexcept;

  MfHardwareH264EncoderFrameSubmitResult SubmitSyntheticNv12Frame(
      ID3D11Device* d3d_device,
      int64_t pts_ns,
      int64_t duration_ns);
  MfHardwareH264EncoderFrameSubmitResult SubmitNv12Texture(
      ID3D11Texture2D* nv12_texture,
      int64_t pts_ns,
      int64_t duration_ns);
  MfHardwareH264EncoderDrainResult DrainSyntheticAvailableOutput(
      uint32_t timeout_ms);
  MfHardwareH264EncoderDrainResult DrainSyntheticEncodedOutput(
      uint32_t timeout_ms);

  bool IsConfigured() const noexcept;
  IMFTransform* transform() const noexcept;
  const MfHardwareH264EncoderSessionInfo& info() const noexcept;
  size_t pending_input_sample_count() const noexcept;

 private:
  MfHardwareH264EncoderDrainResult DrainSyntheticOutput(uint32_t timeout_ms,
                                                        bool request_drain);
  void ReleaseOldestPendingInputSample() noexcept;
  void ReleaseAllPendingInputSamples() noexcept;

  bool mf_started_ = false;
  bool configured_ = false;
  bool stream_started_ = false;
  bool drain_command_sent_ = false;
  IMFTransform* transform_ = nullptr;
  IMFDXGIDeviceManager* device_manager_ = nullptr;
  ID3D11Device* d3d_device_ = nullptr;
  std::deque<IMFSample*> pending_input_samples_;
  uint32_t device_manager_reset_token_ = 0;
  uint64_t submitted_input_frames_ = 0;
  uint64_t drained_output_packets_ = 0;
  uint64_t drained_output_bytes_ = 0;
  MfHardwareH264EncoderSessionInfo info_;
};

const wchar_t* MfHardwareH264EncoderSessionStatusName(
    MfHardwareH264EncoderSessionStatus status) noexcept;
const wchar_t* MfHardwareH264EncoderFrameSubmitStatusName(
    MfHardwareH264EncoderFrameSubmitStatus status) noexcept;
const wchar_t* MfHardwareH264EncoderDrainStatusName(
    MfHardwareH264EncoderDrainStatus status) noexcept;
const wchar_t* MfHardwareH264PacketFormatName(
    MfHardwareH264PacketFormat format) noexcept;
bool IsMfHardwareH264EncoderRuntimeFailure(
    MfHardwareH264EncoderFrameSubmitStatus status) noexcept;
bool IsMfHardwareH264EncoderRuntimeFailure(
    MfHardwareH264EncoderDrainStatus status) noexcept;

}  // namespace olouie::encode
