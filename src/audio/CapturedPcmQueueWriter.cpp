#include "audio/CapturedPcmQueueWriter.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace olouie::audio {
namespace {

CapturedPcmQueueResult Result(CapturedPcmQueueStatus status,
                              std::wstring message,
                              uint32_t prepared_frame_count = 0,
                              size_t queued_byte_count = 0) {
  CapturedPcmQueueResult result;
  result.status = status;
  result.message = std::move(message);
  result.prepared_frame_count = prepared_frame_count;
  result.queued_byte_count = queued_byte_count;
  return result;
}

bool ExpectedByteCount(const PcmStreamFormat& format,
                       uint32_t frame_count,
                       size_t* byte_count) noexcept {
  if (byte_count == nullptr || !format.IsValid()) {
    return false;
  }

  const uint64_t expected = static_cast<uint64_t>(frame_count) *
                            static_cast<uint64_t>(format.block_align);
  if (expected > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }

  *byte_count = static_cast<size_t>(expected);
  return true;
}

}  // namespace

bool CapturedPcmQueueResult::Succeeded() const noexcept {
  return status == CapturedPcmQueueStatus::Success;
}

CapturedPcmQueueWriter::CapturedPcmQueueWriter(
    PreparedPcmQueue* queue,
    PcmStreamFormat input_format,
    uint32_t output_sample_rate)
    : queue_(queue),
      input_format_(input_format),
      output_sample_rate_(output_sample_rate),
      resampler_(std::make_unique<StreamingPcmResampler>(
          input_format, output_sample_rate, input_format.channel_count)) {}

CapturedPcmQueueResult CapturedPcmQueueWriter::QueueCapturedPcm(
    const PcmStreamFormat& captured_format,
    const PcmPacketInfo& packet,
    int64_t pts_ns,
    std::span<const std::byte> pcm_bytes) {
  if (queue_ == nullptr || !input_format_.IsValid() ||
      output_sample_rate_ == 0 || resampler_ == nullptr ||
      !resampler_->IsConfigured()) {
    return Result(CapturedPcmQueueStatus::InvalidConfig,
                  L"Captured PCM queue writer needs a queue and valid format.");
  }

  if (!packet.timing.IsValid() || pts_ns < 0) {
    return Result(CapturedPcmQueueStatus::InvalidPacket,
                  L"Captured PCM packet timing is invalid.");
  }

  if (!captured_format.IsValid() ||
      !((captured_format.encoding == PcmSampleEncoding::Float &&
         captured_format.bits_per_sample == 32) ||
        (captured_format.encoding == PcmSampleEncoding::SignedInteger &&
         captured_format.bits_per_sample == 16))) {
    return Result(CapturedPcmQueueStatus::InvalidPacket,
                  L"Captured PCM packet format is incompatible with the "
                  L"encode track channel layout.");
  }

  if (!SamePcmStreamFormat(captured_format,
                           resampler_->input_format())) {
    resampler_ = std::make_unique<StreamingPcmResampler>(
        captured_format, output_sample_rate_, input_format_.channel_count);
    if (!resampler_->IsConfigured()) {
      ++stats_.prepare_failure_count;
      return Result(CapturedPcmQueueStatus::PrepareFailed,
                    L"Could not reconfigure PCM conversion for a changed "
                    L"capture format.");
    }
    ++stats_.input_format_change_count;
  }

  ++stats_.attempted_packet_count;

  std::span<const std::byte> bytes_to_prepare = pcm_bytes;
  std::vector<std::byte> silence;
  if (packet.silent) {
    size_t silence_size = 0;
    if (!ExpectedByteCount(captured_format, packet.timing.frame_count,
                           &silence_size)) {
      ++stats_.prepare_failure_count;
      return Result(CapturedPcmQueueStatus::PrepareFailed,
                    L"Could not size silent PCM packet.");
    }

    silence.assign(silence_size, std::byte{0});
    bytes_to_prepare = silence;
    ++stats_.silent_packet_count;
  }

  PreparedPcmBuffer prepared;
  const auto prepare = resampler_->Convert(
      packet.timing.frame_count, bytes_to_prepare, &prepared);
  if (!prepare.Succeeded()) {
    ++stats_.prepare_failure_count;
    return Result(CapturedPcmQueueStatus::PrepareFailed, prepare.message);
  }

  if (!prepared.IsValid()) {
    ++stats_.prepare_failure_count;
    return Result(CapturedPcmQueueStatus::PrepareFailed,
                  L"Prepared PCM output was invalid.");
  }

  ++stats_.prepared_block_count;
  if (captured_format.sample_rate != output_sample_rate_) {
    ++stats_.resampled_packet_count;
    stats_.resampled_frame_count += prepared.frame_count;
  }

  const AacPcmInput input{pts_ns,
                          AudioFramesToNs(prepared.frame_count,
                                          output_sample_rate_),
                          prepared.frame_count};
  const auto queued = queue_->TryPush(input, prepared.data);
  if (!queued.Succeeded()) {
    ++stats_.queue_rejection_count;
    return Result(CapturedPcmQueueStatus::QueueRejected, queued.message,
                  prepared.frame_count, prepared.data.size());
  }

  ++stats_.queued_block_count;
  stats_.queued_frame_count += prepared.frame_count;
  return Result(CapturedPcmQueueStatus::Success, L"",
                prepared.frame_count, prepared.data.size());
}

const CapturedPcmQueueStats& CapturedPcmQueueWriter::stats() const noexcept {
  return stats_;
}

}  // namespace olouie::audio
