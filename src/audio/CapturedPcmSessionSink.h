#pragma once

#include <cstdint>

#include "audio/AudioSourceSessionDispatcher.h"
#include "audio/CapturedPcmSink.h"

namespace olouie::audio {

struct CapturedPcmSessionSinkStats {
  uint64_t received_packet_count = 0;
  uint64_t queued_packet_count = 0;
  uint64_t invalid_packet_count = 0;
  uint64_t invalid_config_count = 0;
  uint64_t route_error_count = 0;
  uint64_t queue_error_count = 0;
};

class CapturedPcmSessionSink final : public ICapturedPcmSink {
 public:
  explicit CapturedPcmSessionSink(
      AudioSourceSessionDispatcher* dispatcher,
      int64_t qpc_origin_ns = 0);

  bool IsConfigured() const noexcept;

  CapturedPcmSinkResult OnCapturedPcm(
      const CapturedPcmPacket& packet) override;

  const CapturedPcmSessionSinkStats& stats() const noexcept;
  const AudioSourceSessionDispatchResult& last_dispatch_result()
      const noexcept;

 private:
  AudioSourceSessionDispatcher* dispatcher_ = nullptr;
  int64_t qpc_origin_ns_ = 0;
  CapturedPcmSessionSinkStats stats_;
  AudioSourceSessionDispatchResult last_dispatch_;
};

}  // namespace olouie::audio
