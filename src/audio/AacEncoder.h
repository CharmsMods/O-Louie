#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "audio/AudioTrackPlan.h"
#include "audio/PcmAudio.h"

struct IMFTransform;

namespace olouie::audio {

enum class AacEncoderStatus {
  Success,
  InvalidConfig,
  BackendUnavailable,
  BackendRejectedConfig,
  InvalidState,
  InvalidInput,
  BackendError,
};

struct AacEncoderConfig {
  AudioTrack track;
  record::TrackDefinition packet_track;
  PcmStreamFormat input_format;
  uint32_t output_sample_rate = 48000;
  uint16_t output_channel_count = 0;
  uint32_t bitrate_bps = 192000;
  uint32_t aac_frame_samples = 1024;
};

struct AacEncoderOutputMetadata {
  uint32_t sample_rate = 0;
  uint16_t channel_count = 0;
  uint32_t bitrate_bps = 0;
  uint32_t frame_samples = 0;
  uint32_t payload_type = 0;
  uint32_t profile_level_indication = 0;
  uint32_t audio_object_type = 0;
  std::vector<uint8_t> audio_specific_config;

  bool IsReady() const noexcept;
};

struct AacEncoderResult {
  AacEncoderStatus status = AacEncoderStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct AacPcmInput {
  int64_t pts_ns = 0;
  int64_t duration_ns = 0;
  uint32_t frame_count = 0;
};

struct EncodedAacPacket {
  uint32_t track_id = 0;
  int64_t pts_ns = 0;
  int64_t dts_ns = 0;
  int64_t duration_ns = 0;
  std::vector<std::byte> data;

  bool IsValid() const noexcept;
};

AacEncoderConfig MakeAacEncoderConfig(const AudioTrack& track,
                                      const PcmStreamFormat& input_format,
                                      uint32_t bitrate_bps);

AacEncoderResult ValidateAacEncoderConfig(const AacEncoderConfig& config);

class IAacEncoder {
 public:
  virtual ~IAacEncoder() = default;

  virtual AacEncoderResult SubmitPcm(const AacPcmInput& input,
                                     std::span<const std::byte> pcm_bytes) = 0;
  virtual AacEncoderResult DrainAvailable(
      std::vector<EncodedAacPacket>* packets) = 0;
  virtual AacEncoderResult Flush(std::vector<EncodedAacPacket>* packets) = 0;
};

class AacEncoder final : public IAacEncoder {
 public:
  AacEncoder() = default;
  ~AacEncoder() override;

  AacEncoder(const AacEncoder&) = delete;
  AacEncoder& operator=(const AacEncoder&) = delete;

  AacEncoderResult Initialize(const AacEncoderConfig& config);
  AacEncoderResult SubmitPcm(const AacPcmInput& input,
                             std::span<const std::byte> pcm_bytes) override;
  AacEncoderResult DrainAvailable(
      std::vector<EncodedAacPacket>* packets) override;
  AacEncoderResult Flush(std::vector<EncodedAacPacket>* packets) override;
  bool IsInitialized() const noexcept;
  const std::wstring& backend_name() const noexcept;
  const AacEncoderOutputMetadata& output_metadata() const noexcept;

 private:
  void Reset() noexcept;

  bool mf_started_ = false;
  bool initialized_ = false;
  bool end_of_stream_ = false;
  IMFTransform* transform_ = nullptr;
  AacEncoderConfig config_;
  uint32_t output_buffer_size_ = 0;
  bool output_stream_provides_samples_ = false;
  std::wstring backend_name_;
  AacEncoderOutputMetadata output_metadata_;
};

}  // namespace olouie::audio
