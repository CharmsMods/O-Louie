#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/AudioEncodeSession.h"
#include "audio/AudioSourceRouter.h"
#include "audio/AudioSourceSessionDispatcher.h"
#include "audio/AudioTrackPlan.h"
#include "audio/CapturedPcmSessionSink.h"
#include "audio/CapturedPcmSink.h"

namespace olouie::audio {

class AudioCaptureEncodeBridge final {
 public:
  AudioCaptureEncodeBridge(const AudioTrackPlan& plan,
                           AudioEncodeSession* session,
                           int64_t qpc_origin_ns = 0);

  AudioCaptureEncodeBridge(const AudioCaptureEncodeBridge&) = delete;
  AudioCaptureEncodeBridge& operator=(const AudioCaptureEncodeBridge&) =
      delete;
  AudioCaptureEncodeBridge(AudioCaptureEncodeBridge&&) = delete;
  AudioCaptureEncodeBridge& operator=(AudioCaptureEncodeBridge&&) = delete;

  bool IsConfigured() const noexcept;

  ICapturedPcmSink* captured_pcm_sink() noexcept;
  CapturedPcmSessionSink& session_sink() noexcept;
  const CapturedPcmSessionSink& session_sink() const noexcept;

  AudioEncodeSessionResult DrainQueuedBlocks(size_t max_total_blocks);
  AudioEncodeSessionResult DrainAllQueuedBlocks();

  const CapturedPcmSessionSinkStats& sink_stats() const noexcept;
  const AudioSourceSessionDispatchResult& last_dispatch_result()
      const noexcept;
  const AudioSourceRouter& router() const noexcept;

 private:
  AudioSourceRouter router_;
  AudioEncodeSession* session_ = nullptr;
  AudioSourceSessionDispatcher dispatcher_;
  CapturedPcmSessionSink sink_;
};

}  // namespace olouie::audio
