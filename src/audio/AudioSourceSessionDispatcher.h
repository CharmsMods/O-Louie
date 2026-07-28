#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "audio/AudioEncodeSession.h"
#include "audio/AudioSourceRouter.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

enum class AudioSourceSessionDispatchStatus {
  Success,
  InvalidConfig,
  RouteError,
  QueueError,
};

struct AudioSourceSessionDispatchResult {
  AudioSourceSessionDispatchStatus status =
      AudioSourceSessionDispatchStatus::InvalidConfig;
  std::wstring message;
  AudioSourceRouteStatus route_status = AudioSourceRouteStatus::InvalidPlan;
  AudioEncodeSessionStatus queue_status =
      AudioEncodeSessionStatus::InvalidConfig;
  uint32_t track_id = 0;

  bool Succeeded() const noexcept;
};

class AudioSourceSessionDispatcher final {
 public:
  AudioSourceSessionDispatcher(const AudioSourceRouter* router,
                               AudioEncodeSession* session);

  bool IsConfigured() const noexcept;

  AudioSourceSessionDispatchResult QueueCapturedPcm(
      CapturedAudioSource source,
      const PcmStreamFormat& format,
      const PcmPacketInfo& packet,
      int64_t pts_ns,
      std::span<const std::byte> pcm_bytes);

 private:
  const AudioSourceRouter* router_ = nullptr;
  AudioEncodeSession* session_ = nullptr;
};

}  // namespace olouie::audio
