#include "audio/AacEncoder.h"

#include <combaseapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmreg.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace olouie::audio {
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

AacEncoderResult Result(AacEncoderStatus status, std::wstring message) {
  AacEncoderResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

constexpr size_t kHeAacUserDataPrefixBytes =
    sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX);
static_assert(kHeAacUserDataPrefixBytes == 12);

bool ReadWord(const std::vector<uint8_t>& bytes, size_t offset,
              uint16_t* value) noexcept {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < sizeof(uint16_t)) {
    return false;
  }
  std::memcpy(value, bytes.data() + offset, sizeof(uint16_t));
  return true;
}

bool ReadAacOutputMetadata(IMFMediaType* media_type,
                           const AacEncoderConfig& config,
                           AacEncoderOutputMetadata* metadata) {
  if (media_type == nullptr || metadata == nullptr) {
    return false;
  }

  AacEncoderOutputMetadata built;
  built.sample_rate = config.output_sample_rate;
  built.channel_count = config.output_channel_count;
  built.bitrate_bps = config.bitrate_bps;
  built.frame_samples = config.aac_frame_samples;
  built.audio_object_type = 2;

  UINT32 value = 0;
  if (SUCCEEDED(media_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                      &value))) {
    built.sample_rate = value;
  }
  if (SUCCEEDED(media_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value)) &&
      value <= std::numeric_limits<uint16_t>::max()) {
    built.channel_count = static_cast<uint16_t>(value);
  }
  if (SUCCEEDED(media_type->GetUINT32(MF_MT_AVG_BITRATE, &value))) {
    built.bitrate_bps = value;
  }
  if (SUCCEEDED(media_type->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &value))) {
    built.payload_type = value;
  }
  if (SUCCEEDED(media_type->GetUINT32(
          MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, &value))) {
    built.profile_level_indication = value;
  }

  UINT32 user_data_size = 0;
  if (FAILED(media_type->GetBlobSize(MF_MT_USER_DATA, &user_data_size)) ||
      user_data_size <= kHeAacUserDataPrefixBytes) {
    return false;
  }

  std::vector<uint8_t> user_data(user_data_size);
  UINT32 copied = 0;
  if (FAILED(media_type->GetBlob(MF_MT_USER_DATA, user_data.data(),
                                 user_data_size, &copied)) ||
      copied != user_data_size) {
    return false;
  }

  uint16_t payload_type = 0;
  uint16_t profile_level = 0;
  uint16_t struct_type = 0;
  if (!ReadWord(user_data, 0, &payload_type) ||
      !ReadWord(user_data, 2, &profile_level) ||
      !ReadWord(user_data, 4, &struct_type) || struct_type != 0) {
    return false;
  }

  built.payload_type = payload_type;
  built.profile_level_indication = profile_level;
  built.audio_specific_config.assign(
      user_data.begin() + kHeAacUserDataPrefixBytes, user_data.end());
  if (!built.IsReady()) {
    return false;
  }

  *metadata = std::move(built);
  return true;
}

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

bool HasExpectedPcmLayout(const PcmStreamFormat& format) noexcept {
  const uint32_t bytes_per_sample = format.bits_per_sample / 8;
  const uint32_t expected_block_align =
      static_cast<uint32_t>(format.channel_count) * bytes_per_sample;
  const uint64_t expected_average_bytes =
      static_cast<uint64_t>(format.sample_rate) * expected_block_align;

  return format.bits_per_sample % 8 == 0 &&
         format.block_align == expected_block_align &&
         expected_average_bytes <= std::numeric_limits<uint32_t>::max() &&
         format.average_bytes_per_second ==
             static_cast<uint32_t>(expected_average_bytes);
}

uint32_t AacAverageBytesPerSecond(uint32_t bitrate_bps) noexcept {
  const uint64_t bytes_per_second =
      (static_cast<uint64_t>(bitrate_bps) + 7u) / 8u;
  if (bytes_per_second > std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(bytes_per_second);
}

int64_t NsToHns(int64_t ns) noexcept {
  return ns / 100;
}

int64_t HnsToNs(int64_t hns) noexcept {
  if (hns > std::numeric_limits<int64_t>::max() / 100) {
    return std::numeric_limits<int64_t>::max();
  }
  if (hns < std::numeric_limits<int64_t>::min() / 100) {
    return std::numeric_limits<int64_t>::min();
  }
  return hns * 100;
}

std::wstring ActivateName(IMFActivate* activate) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  if (activate != nullptr &&
      SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                             &value, &length)) &&
      value != nullptr) {
    std::wstring name(value, length);
    CoTaskMemFree(value);
    if (!name.empty()) {
      return name;
    }
  }

  return L"Media Foundation AAC encoder";
}

bool ExpectedPcmByteSize(const PcmStreamFormat& format, uint32_t frame_count,
                         size_t* expected_size) noexcept {
  const uint64_t size = static_cast<uint64_t>(frame_count) *
                        static_cast<uint64_t>(format.block_align);
  if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }

  *expected_size = static_cast<size_t>(size);
  return true;
}

HRESULT CreatePcmInputType(const AacEncoderConfig& config,
                           IMFMediaType** media_type) {
  ComPtr<IMFMediaType> type;
  HRESULT result = MFCreateMediaType(type.put());
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                 config.input_format.channel_count);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                 config.input_format.sample_rate);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                                 config.input_format.bits_per_sample);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                 config.input_format.block_align);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                 config.input_format.average_bytes_per_second);
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

HRESULT CreateAacOutputType(const AacEncoderConfig& config,
                            IMFMediaType** media_type) {
  ComPtr<IMFMediaType> type;
  HRESULT result = MFCreateMediaType(type.put());
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                 config.output_channel_count);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                 config.output_sample_rate);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                 AacAverageBytesPerSecond(config.bitrate_bps));
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_bps);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
  if (FAILED(result)) {
    return result;
  }

  result = type.get()->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
                                 0x29);
  if (FAILED(result)) {
    return result;
  }

  *media_type = type.Detach();
  return S_OK;
}

HRESULT CreateInputSample(const AacPcmInput& input,
                          std::span<const std::byte> pcm_bytes,
                          IMFSample** sample) {
  ComPtr<IMFSample> built_sample;
  HRESULT result = MFCreateSample(built_sample.put());
  if (FAILED(result)) {
    return result;
  }

  ComPtr<IMFMediaBuffer> buffer;
  result = MFCreateMemoryBuffer(static_cast<DWORD>(pcm_bytes.size()),
                                buffer.put());
  if (FAILED(result)) {
    return result;
  }

  BYTE* destination = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  result = buffer.get()->Lock(&destination, &max_length, &current_length);
  if (FAILED(result)) {
    return result;
  }

  if (!pcm_bytes.empty()) {
    std::memcpy(destination, pcm_bytes.data(), pcm_bytes.size());
  }
  buffer.get()->Unlock();

  result = buffer.get()->SetCurrentLength(static_cast<DWORD>(pcm_bytes.size()));
  if (FAILED(result)) {
    return result;
  }

  result = built_sample.get()->AddBuffer(buffer.get());
  if (FAILED(result)) {
    return result;
  }

  result = built_sample.get()->SetSampleTime(NsToHns(input.pts_ns));
  if (FAILED(result)) {
    return result;
  }

  result = built_sample.get()->SetSampleDuration(NsToHns(input.duration_ns));
  if (FAILED(result)) {
    return result;
  }

  *sample = built_sample.Detach();
  return S_OK;
}

HRESULT CreateOutputSample(uint32_t buffer_size, IMFSample** sample) {
  ComPtr<IMFSample> built_sample;
  HRESULT result = MFCreateSample(built_sample.put());
  if (FAILED(result)) {
    return result;
  }

  ComPtr<IMFMediaBuffer> buffer;
  result = MFCreateMemoryBuffer(buffer_size, buffer.put());
  if (FAILED(result)) {
    return result;
  }

  result = built_sample.get()->AddBuffer(buffer.get());
  if (FAILED(result)) {
    return result;
  }

  *sample = built_sample.Detach();
  return S_OK;
}

HRESULT SampleToPacket(IMFSample* sample, uint32_t track_id,
                       EncodedAacPacket* packet) {
  if (sample == nullptr || packet == nullptr) {
    return E_POINTER;
  }

  LONGLONG sample_time = 0;
  if (FAILED(sample->GetSampleTime(&sample_time))) {
    sample_time = 0;
  }

  LONGLONG sample_duration = 0;
  if (FAILED(sample->GetSampleDuration(&sample_duration))) {
    sample_duration = 0;
  }

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT result = sample->ConvertToContiguousBuffer(buffer.put());
  if (FAILED(result)) {
    return result;
  }

  DWORD current_length = 0;
  result = buffer.get()->GetCurrentLength(&current_length);
  if (FAILED(result)) {
    return result;
  }

  EncodedAacPacket built;
  built.track_id = track_id;
  built.pts_ns = HnsToNs(sample_time);
  built.dts_ns = built.pts_ns;
  built.duration_ns = HnsToNs(sample_duration);
  built.data.assign(current_length, std::byte{0});

  if (current_length > 0) {
    BYTE* source = nullptr;
    DWORD max_length = 0;
    DWORD locked_length = 0;
    result = buffer.get()->Lock(&source, &max_length, &locked_length);
    if (FAILED(result)) {
      return result;
    }

    const DWORD copy_length = std::min(current_length, locked_length);
    std::memcpy(built.data.data(), source, copy_length);
    buffer.get()->Unlock();
    built.data.resize(copy_length);
  }

  *packet = std::move(built);
  return S_OK;
}

AacEncoderResult TryConfigureActivate(const AacEncoderConfig& config,
                                      IMFActivate* activate,
                                      IMFTransform** transform,
                                      std::wstring* backend_name,
                                      AacEncoderOutputMetadata* metadata) {
  ComPtr<IMFTransform> candidate;
  HRESULT result =
      activate->ActivateObject(__uuidof(IMFTransform),
                               reinterpret_cast<void**>(candidate.put()));
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"Could not activate AAC encoder MFT (" +
                      HResultToHex(result) + L").");
  }

  ComPtr<IMFMediaType> input_type;
  result = CreatePcmInputType(config, input_type.put());
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"Could not create AAC input media type (" +
                      HResultToHex(result) + L").");
  }

  ComPtr<IMFMediaType> output_type;
  result = CreateAacOutputType(config, output_type.put());
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"Could not create AAC output media type (" +
                      HResultToHex(result) + L").");
  }

  result = candidate.get()->SetOutputType(0, output_type.get(), 0);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"AAC encoder rejected output media type (" +
                      HResultToHex(result) + L").");
  }

  result = candidate.get()->SetInputType(0, input_type.get(), 0);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"AAC encoder rejected input media type (" +
                      HResultToHex(result) + L").");
  }

  ComPtr<IMFMediaType> configured_output_type;
  IMFMediaType* metadata_type = output_type.get();
  if (SUCCEEDED(candidate.get()->GetOutputCurrentType(
          0, configured_output_type.put())) &&
      configured_output_type.get() != nullptr) {
    metadata_type = configured_output_type.get();
  }
  if (!ReadAacOutputMetadata(metadata_type, config, metadata)) {
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"AAC encoder did not expose usable raw AAC decoder "
                  L"configuration metadata.");
  }

  *backend_name = ActivateName(activate);
  *transform = candidate.Detach();
  return Result(AacEncoderStatus::Success, L"");
}

AacEncoderResult CreateMediaFoundationAacEncoder(
    const AacEncoderConfig& config, IMFTransform** transform,
    std::wstring* backend_name, AacEncoderOutputMetadata* metadata) {
  MFT_REGISTER_TYPE_INFO input_info;
  input_info.guidMajorType = MFMediaType_Audio;
  input_info.guidSubtype = MFAudioFormat_PCM;

  MFT_REGISTER_TYPE_INFO output_info;
  output_info.guidMajorType = MFMediaType_Audio;
  output_info.guidSubtype = MFAudioFormat_AAC;

  ActivateList activates;
  HRESULT result = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                             MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                             &input_info, &output_info, activates.put(),
                             activates.count_put());
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendUnavailable,
                  L"Could not enumerate AAC encoder MFTs (" +
                      HResultToHex(result) + L").");
  }

  if (activates.count() == 0) {
    return Result(AacEncoderStatus::BackendUnavailable,
                  L"No Media Foundation AAC encoder MFT is available.");
  }

  AacEncoderResult last_rejection =
      Result(AacEncoderStatus::BackendRejectedConfig,
             L"No AAC encoder accepted the requested media types.");
  for (UINT32 index = 0; index < activates.count(); ++index) {
    last_rejection = TryConfigureActivate(config, activates[index], transform,
                                          backend_name, metadata);
    if (last_rejection.Succeeded()) {
      return last_rejection;
    }
  }

  return last_rejection;
}

}  // namespace

bool AacEncoderResult::Succeeded() const noexcept {
  return status == AacEncoderStatus::Success;
}

bool EncodedAacPacket::IsValid() const noexcept {
  return track_id != 0 && duration_ns >= 0 && !data.empty();
}

bool AacEncoderOutputMetadata::IsReady() const noexcept {
  return (sample_rate == 44100 || sample_rate == 48000) &&
         (channel_count == 1 || channel_count == 2 || channel_count == 6) &&
         bitrate_bps > 0 && frame_samples == 1024 && payload_type == 0 &&
         audio_object_type == 2 && !audio_specific_config.empty();
}

AacEncoderConfig MakeAacEncoderConfig(const AudioTrack& track,
                                      const PcmStreamFormat& input_format,
                                      uint32_t bitrate_bps) {
  AacEncoderConfig config;
  config.track = track;
  config.packet_track = track.ToPacketTrack();
  config.input_format = input_format;
  config.output_sample_rate = input_format.sample_rate;
  config.output_channel_count = input_format.channel_count;
  config.bitrate_bps = bitrate_bps;
  return config;
}

AacEncoderResult ValidateAacEncoderConfig(const AacEncoderConfig& config) {
  if (config.track.track_id == 0) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder track id must be nonzero.");
  }

  if (config.track.name.empty()) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder track name must not be empty.");
  }

  if (config.packet_track.track_id != config.track.track_id ||
      config.packet_track.codec_id != record::CodecId::Aac) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder packet track must match the audio track.");
  }

  if (!config.input_format.IsValid()) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder input PCM format is invalid.");
  }

  if (config.input_format.encoding != PcmSampleEncoding::SignedInteger ||
      config.input_format.bits_per_sample != 16) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder input must be prepared signed 16-bit PCM.");
  }

  if (!HasExpectedPcmLayout(config.input_format)) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder input PCM layout is inconsistent.");
  }

  if (config.output_sample_rate == 0 ||
      config.output_sample_rate != config.input_format.sample_rate) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder output sample rate must match input PCM.");
  }

  if (config.output_channel_count == 0 ||
      config.output_channel_count != config.input_format.channel_count) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder output channels must match input PCM.");
  }

  if (config.bitrate_bps == 0) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC encoder bitrate must be greater than zero.");
  }

  if (config.aac_frame_samples != 1024) {
    return Result(AacEncoderStatus::InvalidConfig,
                  L"AAC-LC packetization expects 1024 samples per frame.");
  }

  return Result(AacEncoderStatus::Success, L"");
}

AacEncoder::~AacEncoder() {
  Reset();
}

AacEncoderResult AacEncoder::Initialize(const AacEncoderConfig& config) {
  Reset();

  const auto validation = ValidateAacEncoderConfig(config);
  if (!validation.Succeeded()) {
    return validation;
  }

  HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendUnavailable,
                  L"Could not start Media Foundation (" +
                      HResultToHex(result) + L").");
  }
  mf_started_ = true;

  std::wstring selected_backend;
  IMFTransform* selected_transform = nullptr;
  AacEncoderOutputMetadata selected_metadata;
  auto init_result = CreateMediaFoundationAacEncoder(
      config, &selected_transform, &selected_backend, &selected_metadata);
  if (!init_result.Succeeded()) {
    Reset();
    return init_result;
  }

  transform_ = selected_transform;
  config_ = config;

  MFT_OUTPUT_STREAM_INFO output_info{};
  result = transform_->GetOutputStreamInfo(0, &output_info);
  if (FAILED(result)) {
    Reset();
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"Could not query AAC output stream info (" +
                      HResultToHex(result) + L").");
  }

  output_stream_provides_samples_ =
      (output_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
  output_buffer_size_ = output_info.cbSize;
  if (output_buffer_size_ == 0) {
    output_buffer_size_ = 16384;
  }

  result =
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(result)) {
    Reset();
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"AAC encoder rejected begin-streaming message (" +
                      HResultToHex(result) + L").");
  }

  result =
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(result)) {
    Reset();
    return Result(AacEncoderStatus::BackendRejectedConfig,
                  L"AAC encoder rejected start-of-stream message (" +
                      HResultToHex(result) + L").");
  }

  backend_name_ = std::move(selected_backend);
  output_metadata_ = std::move(selected_metadata);
  initialized_ = true;
  end_of_stream_ = false;
  return init_result;
}

AacEncoderResult AacEncoder::SubmitPcm(const AacPcmInput& input,
                                       std::span<const std::byte> pcm_bytes) {
  if (!initialized_ || transform_ == nullptr) {
    return Result(AacEncoderStatus::InvalidState,
                  L"AAC encoder is not initialized.");
  }

  if (end_of_stream_) {
    return Result(AacEncoderStatus::InvalidState,
                  L"AAC encoder has already been flushed.");
  }

  if (input.frame_count == 0 || input.duration_ns <= 0) {
    return Result(AacEncoderStatus::InvalidInput,
                  L"AAC input needs positive frame count and duration.");
  }

  size_t expected_size = 0;
  if (!ExpectedPcmByteSize(config_.input_format, input.frame_count,
                           &expected_size) ||
      pcm_bytes.size() != expected_size ||
      pcm_bytes.size() >
          static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
    return Result(AacEncoderStatus::InvalidInput,
                  L"AAC input PCM byte count does not match frame count.");
  }

  ComPtr<IMFSample> sample;
  HRESULT result = CreateInputSample(input, pcm_bytes, sample.put());
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendError,
                  L"Could not create AAC input sample (" +
                      HResultToHex(result) + L").");
  }

  result = transform_->ProcessInput(0, sample.get(), 0);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendError,
                  L"AAC encoder rejected PCM input (" +
                      HResultToHex(result) + L").");
  }

  return Result(AacEncoderStatus::Success, L"");
}

AacEncoderResult AacEncoder::DrainAvailable(
    std::vector<EncodedAacPacket>* packets) {
  if (packets == nullptr) {
    return Result(AacEncoderStatus::InvalidInput,
                  L"AAC drain needs a packet destination.");
  }
  packets->clear();

  if (!initialized_ || transform_ == nullptr) {
    return Result(AacEncoderStatus::InvalidState,
                  L"AAC encoder is not initialized.");
  }

  for (uint32_t drain_iteration = 0; drain_iteration < 128;
       ++drain_iteration) {
    ComPtr<IMFSample> caller_sample;
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;

    if (!output_stream_provides_samples_) {
      HRESULT sample_result =
          CreateOutputSample(output_buffer_size_, caller_sample.put());
      if (FAILED(sample_result)) {
        return Result(AacEncoderStatus::BackendError,
                      L"Could not create AAC output sample (" +
                          HResultToHex(sample_result) + L").");
      }
      output.pSample = caller_sample.get();
    }

    DWORD status = 0;
    HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents != nullptr) {
      output.pEvents->Release();
      output.pEvents = nullptr;
    }

    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (output_stream_provides_samples_ && output.pSample != nullptr) {
        output.pSample->Release();
      }
      return Result(AacEncoderStatus::Success, L"");
    }

    if (FAILED(result)) {
      if (output_stream_provides_samples_ && output.pSample != nullptr) {
        output.pSample->Release();
      }
      return Result(AacEncoderStatus::BackendError,
                    L"AAC encoder output drain failed (" +
                        HResultToHex(result) + L").");
    }

    EncodedAacPacket packet;
    if (output.pSample != nullptr &&
        SUCCEEDED(SampleToPacket(output.pSample, config_.track.track_id,
                                 &packet)) &&
        packet.IsValid()) {
      packets->push_back(std::move(packet));
    }

    if (output_stream_provides_samples_ && output.pSample != nullptr) {
      output.pSample->Release();
    }
  }

  return Result(AacEncoderStatus::BackendError,
                L"AAC encoder output drain did not settle.");
}

AacEncoderResult AacEncoder::Flush(std::vector<EncodedAacPacket>* packets) {
  if (!initialized_ || transform_ == nullptr) {
    return Result(AacEncoderStatus::InvalidState,
                  L"AAC encoder is not initialized.");
  }

  HRESULT result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,
                                              0);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendError,
                  L"AAC encoder rejected end-of-stream message (" +
                      HResultToHex(result) + L").");
  }

  result = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  if (FAILED(result)) {
    return Result(AacEncoderStatus::BackendError,
                  L"AAC encoder rejected drain command (" +
                      HResultToHex(result) + L").");
  }

  end_of_stream_ = true;
  return DrainAvailable(packets);
}

bool AacEncoder::IsInitialized() const noexcept {
  return initialized_;
}

const std::wstring& AacEncoder::backend_name() const noexcept {
  return backend_name_;
}

const AacEncoderOutputMetadata& AacEncoder::output_metadata() const noexcept {
  return output_metadata_;
}

void AacEncoder::Reset() noexcept {
  if (transform_ != nullptr) {
    transform_->Release();
    transform_ = nullptr;
  }

  initialized_ = false;
  end_of_stream_ = false;
  config_ = {};
  output_buffer_size_ = 0;
  output_stream_provides_samples_ = false;
  backend_name_.clear();
  output_metadata_ = {};

  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
}

}  // namespace olouie::audio
