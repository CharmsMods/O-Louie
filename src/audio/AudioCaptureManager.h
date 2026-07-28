#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/AudioSource.h"
#include "audio/AudioTrackPlan.h"
#include "audio/CapturedPcmSink.h"

namespace olouie::audio {

enum class AudioCaptureSourceRuntime {
  SystemLoopback,
  Microphone,
  ProcessLoopback,
};

enum class AudioCaptureSourceSupport {
  Supported,
  Deferred,
};

struct AudioCaptureSourceBinding {
  CapturedAudioSource source;
  AudioCaptureSourceRuntime runtime =
      AudioCaptureSourceRuntime::SystemLoopback;
  AudioCaptureSourceSupport support = AudioCaptureSourceSupport::Deferred;
  uint32_t track_id = 0;
  ICapturedPcmSink* sink = nullptr;
};

enum class AudioCaptureManagerStatus {
  Success,
  InvalidPlan,
  MissingSink,
  UnsupportedPlan,
  DuplicateSource,
};

struct AudioCaptureManagerResult {
  AudioCaptureManagerStatus status = AudioCaptureManagerStatus::InvalidPlan;
  std::wstring message;

  bool Succeeded() const noexcept;
};

class AudioCaptureManager final {
 public:
  AudioCaptureManagerResult Configure(const AudioTrackPlan& plan,
                                      ICapturedPcmSink* sink);
  void Reset() noexcept;

  bool IsConfigured() const noexcept;
  size_t source_count() const noexcept;
  size_t supported_source_count() const noexcept;
  size_t deferred_source_count() const noexcept;
  bool has_deferred_mixed_track() const noexcept;
  ICapturedPcmSink* sink() const noexcept;

  const std::vector<AudioCaptureSourceBinding>& sources() const noexcept;
  const AudioCaptureSourceBinding* FindSource(
      CapturedAudioSource source) const noexcept;

 private:
  bool configured_ = false;
  bool deferred_mixed_track_ = false;
  ICapturedPcmSink* sink_ = nullptr;
  std::vector<AudioCaptureSourceBinding> sources_;
};

bool AudioCaptureSourceIsSupported(
    const AudioCaptureSourceBinding& binding) noexcept;
const wchar_t* AudioCaptureSourceRuntimeName(
    AudioCaptureSourceRuntime runtime) noexcept;
const wchar_t* AudioCaptureSourceSupportName(
    AudioCaptureSourceSupport support) noexcept;

}  // namespace olouie::audio
