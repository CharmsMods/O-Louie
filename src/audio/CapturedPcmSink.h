#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "audio/AudioSource.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

struct CapturedPcmPacket {
  CapturedAudioSource source;
  PcmStreamFormat format;
  PcmPacketInfo packet;
  std::span<const std::byte> pcm_bytes;

  bool IsValid() const noexcept;
};

enum class CapturedPcmSinkStatus {
  Success,
  InvalidPacket,
  InvalidConfig,
  RouteError,
  QueueError,
  SinkError,
};

struct CapturedPcmSinkResult {
  CapturedPcmSinkStatus status = CapturedPcmSinkStatus::SinkError;
  std::wstring message;

  bool Succeeded() const noexcept;
};

class ICapturedPcmSink {
 public:
  virtual ~ICapturedPcmSink() = default;

  virtual CapturedPcmSinkResult OnCapturedPcm(
      const CapturedPcmPacket& packet) = 0;
};

CapturedPcmSinkResult DispatchCapturedPcm(ICapturedPcmSink* sink,
                                          const CapturedPcmPacket& packet);

}  // namespace olouie::audio
