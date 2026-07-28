#include "audio/CapturedPcmSink.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

CapturedPcmSinkResult Result(CapturedPcmSinkStatus status,
                             std::wstring message) {
  CapturedPcmSinkResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool ExpectedPcmByteCount(const PcmStreamFormat& format,
                          const PcmPacketInfo& packet,
                          size_t* byte_count) noexcept {
  if (byte_count == nullptr || !format.IsValid() ||
      !packet.timing.IsValid()) {
    return false;
  }

  const auto expected =
      static_cast<uint64_t>(packet.timing.frame_count) * format.block_align;
  if (expected > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }

  *byte_count = static_cast<size_t>(expected);
  return true;
}

}  // namespace

bool CapturedPcmPacket::IsValid() const noexcept {
  if (!IsCapturedAudioSourceValid(source)) {
    return false;
  }

  size_t expected_bytes = 0;
  if (!ExpectedPcmByteCount(format, packet, &expected_bytes)) {
    return false;
  }

  if (packet.silent) {
    return pcm_bytes.empty() || pcm_bytes.size() == expected_bytes;
  }

  return expected_bytes > 0 && pcm_bytes.size() == expected_bytes;
}

bool CapturedPcmSinkResult::Succeeded() const noexcept {
  return status == CapturedPcmSinkStatus::Success;
}

CapturedPcmSinkResult DispatchCapturedPcm(ICapturedPcmSink* sink,
                                          const CapturedPcmPacket& packet) {
  if (!packet.IsValid()) {
    return Result(CapturedPcmSinkStatus::InvalidPacket,
                  L"Captured PCM packet is invalid.");
  }

  if (sink == nullptr) {
    return Result(CapturedPcmSinkStatus::Success, L"");
  }

  return sink->OnCapturedPcm(packet);
}

}  // namespace olouie::audio
