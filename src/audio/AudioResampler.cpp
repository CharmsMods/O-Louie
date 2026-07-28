#include "audio/AudioResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioResampleResult Result(AudioResampleStatus status, std::wstring message) {
  AudioResampleResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool HasConsistentPcmLayout(const PcmStreamFormat& format) noexcept {
  if (!format.IsValid() || format.bits_per_sample % 8 != 0) {
    return false;
  }

  const uint32_t bytes_per_sample = format.bits_per_sample / 8;
  const uint32_t expected_block_align =
      static_cast<uint32_t>(format.channel_count) * bytes_per_sample;
  const uint64_t expected_average_bytes =
      static_cast<uint64_t>(format.sample_rate) * expected_block_align;

  return format.block_align == expected_block_align &&
         expected_average_bytes <= std::numeric_limits<uint32_t>::max() &&
         format.average_bytes_per_second ==
             static_cast<uint32_t>(expected_average_bytes);
}

bool ExpectedInputSize(const PcmStreamFormat& format, uint32_t frame_count,
                       size_t* expected_size) noexcept {
  const uint64_t size = static_cast<uint64_t>(frame_count) *
                        static_cast<uint64_t>(format.block_align);
  if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }

  *expected_size = static_cast<size_t>(size);
  return true;
}

int16_t FloatToSigned16(float sample) noexcept {
  if (!std::isfinite(sample)) {
    return 0;
  }

  if (sample >= 1.0f) {
    return std::numeric_limits<int16_t>::max();
  }

  if (sample <= -1.0f) {
    return std::numeric_limits<int16_t>::min();
  }

  const float scaled =
      sample >= 0.0f ? sample * 32767.0f : sample * 32768.0f;
  return static_cast<int16_t>(std::lround(scaled));
}

void WriteSigned16Sample(int16_t sample, std::byte* destination) noexcept {
  const auto value = static_cast<uint16_t>(sample);
  destination[0] = static_cast<std::byte>(value & 0x00ffu);
  destination[1] = static_cast<std::byte>((value >> 8u) & 0x00ffu);
}

float ReadFloatSample(const std::byte* source) noexcept {
  float sample = 0.0f;
  std::memcpy(&sample, source, sizeof(sample));
  return sample;
}

int16_t ReadSigned16Sample(const std::byte* source) noexcept {
  int16_t sample = 0;
  std::memcpy(&sample, source, sizeof(sample));
  return sample;
}

bool SupportsInputSamples(const PcmStreamFormat& format) noexcept {
  return (format.encoding == PcmSampleEncoding::SignedInteger &&
          format.bits_per_sample == 16) ||
         (format.encoding == PcmSampleEncoding::Float &&
          format.bits_per_sample == 32);
}

bool SupportsChannelConversion(uint16_t input_channels,
                               uint16_t output_channels) noexcept {
  return input_channels == output_channels ||
         (input_channels <= 2 && output_channels <= 2);
}

float ReadNormalizedSample(const PcmStreamFormat& format,
                           const std::byte* source) noexcept {
  if (format.encoding == PcmSampleEncoding::Float) {
    const float sample = ReadFloatSample(source);
    return std::isfinite(sample) ? sample : 0.0f;
  }
  const int16_t sample = ReadSigned16Sample(source);
  return sample >= 0 ? static_cast<float>(sample) / 32767.0f
                     : static_cast<float>(sample) / 32768.0f;
}

bool CheckedFrameScale(uint64_t frames,
                       uint32_t numerator,
                       uint32_t denominator,
                       uint64_t* scaled) noexcept {
  if (scaled == nullptr || numerator == 0 || denominator == 0 ||
      frames > std::numeric_limits<uint64_t>::max() / numerator) {
    return false;
  }
  *scaled = (frames * numerator) / denominator;
  return true;
}

}  // namespace

bool PreparedPcmBuffer::IsValid() const noexcept {
  return format.IsValid() && frame_count > 0 &&
         data.size() ==
             static_cast<size_t>(frame_count) * format.block_align;
}

bool AudioResampleResult::Succeeded() const noexcept {
  return status == AudioResampleStatus::Success;
}

StreamingPcmResampler::StreamingPcmResampler(
    PcmStreamFormat input_format,
    uint32_t output_sample_rate,
    uint16_t output_channel_count)
    : input_format_(input_format),
      output_sample_rate_(output_sample_rate),
      output_channel_count_(output_channel_count == 0
                                ? input_format.channel_count
                                : output_channel_count) {}

bool StreamingPcmResampler::IsConfigured() const noexcept {
  return HasConsistentPcmLayout(input_format_) &&
         SupportsInputSamples(input_format_) && output_sample_rate_ > 0 &&
         SupportsChannelConversion(input_format_.channel_count,
                                   output_channel_count_);
}

const PcmStreamFormat& StreamingPcmResampler::input_format() const noexcept {
  return input_format_;
}

uint32_t StreamingPcmResampler::output_sample_rate() const noexcept {
  return output_sample_rate_;
}

uint64_t StreamingPcmResampler::total_input_frame_count() const noexcept {
  return total_input_frame_count_;
}

uint64_t StreamingPcmResampler::total_output_frame_count() const noexcept {
  return total_output_frame_count_;
}

AudioResampleResult StreamingPcmResampler::Convert(
    uint32_t input_frame_count,
    std::span<const std::byte> input_bytes,
    PreparedPcmBuffer* output) {
  if (output == nullptr) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"PCM preparation needs an output destination.");
  }
  *output = {};

  if (!IsConfigured() || input_frame_count == 0) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Streaming PCM conversion needs a valid format and "
                  L"positive frame count.");
  }

  size_t expected_size = 0;
  if (!ExpectedInputSize(input_format_, input_frame_count, &expected_size) ||
      input_bytes.size() != expected_size) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Input PCM byte count does not match frame count.");
  }

  const auto output_format = MakeSigned16PcmFormat(
      output_sample_rate_, output_channel_count_);
  if (!output_format.IsValid()) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Could not build the converted PCM output format.");
  }

  if (input_format_.sample_rate == output_sample_rate_ &&
      input_format_.channel_count == output_channel_count_) {
    if (total_input_frame_count_ >
            std::numeric_limits<uint64_t>::max() - input_frame_count ||
        total_output_frame_count_ >
            std::numeric_limits<uint64_t>::max() - input_frame_count) {
      return Result(AudioResampleStatus::InvalidInput,
                    L"Streaming PCM frame position overflowed.");
    }
    const uint64_t output_size =
        static_cast<uint64_t>(input_frame_count) * output_format.block_align;
    if (output_size > std::numeric_limits<size_t>::max()) {
      return Result(AudioResampleStatus::InvalidInput,
                    L"Converted PCM output is too large.");
    }
    output->format = output_format;
    output->frame_count = input_frame_count;
    output->data.assign(static_cast<size_t>(output_size), std::byte{0});

    const size_t sample_count =
        static_cast<size_t>(input_frame_count) * input_format_.channel_count;
    if (input_format_.encoding == PcmSampleEncoding::SignedInteger) {
      output->data.assign(input_bytes.begin(), input_bytes.end());
    } else {
      for (size_t index = 0; index < sample_count; ++index) {
        const float sample = ReadNormalizedSample(
            input_format_, input_bytes.data() + index * sizeof(float));
        WriteSigned16Sample(FloatToSigned16(sample),
                            output->data.data() + index * sizeof(int16_t));
      }
    }
    total_input_frame_count_ += input_frame_count;
    total_output_frame_count_ += input_frame_count;
    return Result(AudioResampleStatus::Success, L"");
  }

  const size_t input_channel_count = input_format_.channel_count;
  const size_t output_channel_count = output_channel_count_;
  std::vector<float> decoded(
      static_cast<size_t>(input_frame_count) * input_channel_count);
  const size_t bytes_per_sample = input_format_.bits_per_sample / 8;
  for (size_t index = 0; index < decoded.size(); ++index) {
    decoded[index] = ReadNormalizedSample(
        input_format_, input_bytes.data() + index * bytes_per_sample);
  }

  const uint64_t input_start = total_input_frame_count_;
  if (input_start >
      std::numeric_limits<uint64_t>::max() - input_frame_count) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Streaming PCM input frame position overflowed.");
  }
  const uint64_t input_end = input_start + input_frame_count;
  uint64_t target_output_frames = 0;
  if (!CheckedFrameScale(input_end, output_sample_rate_,
                         input_format_.sample_rate,
                         &target_output_frames) ||
      target_output_frames < total_output_frame_count_) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Streaming PCM output frame position overflowed.");
  }

  const uint64_t output_frame_count64 =
      target_output_frames - total_output_frame_count_;
  if (output_frame_count64 == 0 ||
      output_frame_count64 > std::numeric_limits<uint32_t>::max() ||
      output_frame_count64 >
          std::numeric_limits<size_t>::max() / output_format.block_align) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"Streaming PCM conversion produced an invalid block "
                  L"size.");
  }

  const size_t history_frame_count =
      history_samples_.size() / input_channel_count;
  const uint64_t history_start = input_start - history_frame_count;
  auto sample_at = [&](uint64_t frame, size_t channel) -> float {
    if (frame < input_start) {
      const size_t history_index =
          static_cast<size_t>(frame - history_start) * input_channel_count +
          channel;
      return history_samples_[history_index];
    }
    const size_t decoded_index =
        static_cast<size_t>(frame - input_start) * input_channel_count +
        channel;
    return decoded[decoded_index];
  };

  output->format = output_format;
  output->frame_count = static_cast<uint32_t>(output_frame_count64);
  output->data.assign(static_cast<size_t>(output_frame_count64) *
                          output_format.block_align,
                      std::byte{0});

  for (uint64_t local_output = 0;
       local_output < output_frame_count64; ++local_output) {
    const uint64_t output_frame =
        total_output_frame_count_ + local_output;
    const long double source_position =
        (static_cast<long double>(output_frame) *
         static_cast<long double>(input_format_.sample_rate)) /
        static_cast<long double>(output_sample_rate_);
    const uint64_t source_frame0 =
        static_cast<uint64_t>(std::floor(source_position));
    const uint64_t source_frame1 =
        std::min(source_frame0 + 1, input_end - 1);
    if (source_frame0 < history_start || source_frame0 >= input_end) {
      return Result(AudioResampleStatus::InvalidInput,
                    L"Streaming PCM resampler lost source history.");
    }
    const float fraction =
        static_cast<float>(source_position - source_frame0);
    auto interpolated_input = [&](size_t channel) {
      const float first = sample_at(source_frame0, channel);
      const float second = sample_at(source_frame1, channel);
      return first + (second - first) * fraction;
    };
    for (size_t channel = 0; channel < output_channel_count; ++channel) {
      float interpolated = 0.0f;
      if (input_channel_count == output_channel_count) {
        interpolated = interpolated_input(channel);
      } else if (input_channel_count == 1) {
        interpolated = interpolated_input(0);
      } else {
        for (size_t input_channel = 0;
             input_channel < input_channel_count; ++input_channel) {
          interpolated += interpolated_input(input_channel);
        }
        interpolated /= static_cast<float>(input_channel_count);
      }
      const size_t output_sample =
          static_cast<size_t>(local_output) * output_channel_count + channel;
      WriteSigned16Sample(
          FloatToSigned16(interpolated),
          output->data.data() + output_sample * sizeof(int16_t));
    }
  }

  const size_t history_limit_frames =
      static_cast<size_t>(
          (input_format_.sample_rate + output_sample_rate_ - 1) /
          output_sample_rate_) +
      2;
  std::vector<float> combined;
  combined.reserve(history_samples_.size() + decoded.size());
  combined.insert(combined.end(), history_samples_.begin(),
                  history_samples_.end());
  combined.insert(combined.end(), decoded.begin(), decoded.end());
  const size_t combined_frame_count =
      combined.size() / input_channel_count;
  const size_t kept_frame_count =
      std::min(history_limit_frames, combined_frame_count);
  const auto kept_begin = combined.end() -
      static_cast<std::ptrdiff_t>(kept_frame_count * input_channel_count);
  history_samples_.assign(kept_begin, combined.end());

  total_input_frame_count_ = input_end;
  total_output_frame_count_ = target_output_frames;
  return Result(AudioResampleStatus::Success, L"");
}

PcmStreamFormat MakeSigned16PcmFormat(uint32_t sample_rate,
                                      uint16_t channel_count) noexcept {
  const uint16_t bits_per_sample = 16;
  if (sample_rate == 0 || channel_count == 0 ||
      channel_count >
          (std::numeric_limits<uint16_t>::max() / (bits_per_sample / 8))) {
    return {};
  }

  const uint16_t block_align =
      static_cast<uint16_t>(channel_count * (bits_per_sample / 8));
  const uint64_t average_bytes_per_second =
      static_cast<uint64_t>(sample_rate) * block_align;
  if (average_bytes_per_second > std::numeric_limits<uint32_t>::max()) {
    return {};
  }

  return MakePcmStreamFormat(sample_rate, channel_count, bits_per_sample,
                             block_align,
                             static_cast<uint32_t>(average_bytes_per_second),
                             PcmSampleEncoding::SignedInteger);
}

AudioResampleResult PreparePcmForAac(
    const PcmStreamFormat& input_format, uint32_t input_frame_count,
    std::span<const std::byte> input_bytes, uint32_t output_sample_rate,
    PreparedPcmBuffer* output) {
  if (output == nullptr) {
    return Result(AudioResampleStatus::InvalidInput,
                  L"PCM preparation needs an output destination.");
  }

  StreamingPcmResampler resampler(input_format, output_sample_rate);
  if (!HasConsistentPcmLayout(input_format) ||
      !SupportsInputSamples(input_format)) {
    *output = {};
    return Result(AudioResampleStatus::UnsupportedConversion,
                  L"PCM conversion supports only interleaved S16 or F32 "
                  L"input.");
  }
  return resampler.Convert(input_frame_count, input_bytes, output);
}

}  // namespace olouie::audio
