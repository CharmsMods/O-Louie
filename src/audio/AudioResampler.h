#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/PcmAudio.h"

namespace olouie::audio {

enum class AudioResampleStatus {
  Success,
  InvalidInput,
  UnsupportedConversion,
  UnsupportedResample,
};

struct PreparedPcmBuffer {
  PcmStreamFormat format;
  uint32_t frame_count = 0;
  std::vector<std::byte> data;

  bool IsValid() const noexcept;
};

struct AudioResampleResult {
  AudioResampleStatus status = AudioResampleStatus::InvalidInput;
  std::wstring message;

  bool Succeeded() const noexcept;
};

class StreamingPcmResampler final {
 public:
  StreamingPcmResampler(PcmStreamFormat input_format,
                        uint32_t output_sample_rate,
                        uint16_t output_channel_count = 0);

  bool IsConfigured() const noexcept;
  const PcmStreamFormat& input_format() const noexcept;
  uint32_t output_sample_rate() const noexcept;
  uint64_t total_input_frame_count() const noexcept;
  uint64_t total_output_frame_count() const noexcept;

  AudioResampleResult Convert(uint32_t input_frame_count,
                              std::span<const std::byte> input_bytes,
                              PreparedPcmBuffer* output);

 private:
  PcmStreamFormat input_format_;
  uint32_t output_sample_rate_ = 0;
  uint16_t output_channel_count_ = 0;
  uint64_t total_input_frame_count_ = 0;
  uint64_t total_output_frame_count_ = 0;
  std::vector<float> history_samples_;
};

PcmStreamFormat MakeSigned16PcmFormat(uint32_t sample_rate,
                                      uint16_t channel_count) noexcept;

AudioResampleResult PreparePcmForAac(
    const PcmStreamFormat& input_format, uint32_t input_frame_count,
    std::span<const std::byte> input_bytes, uint32_t output_sample_rate,
    PreparedPcmBuffer* output);

}  // namespace olouie::audio
