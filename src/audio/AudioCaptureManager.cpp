#include "audio/AudioCaptureManager.h"

#include <string>
#include <utility>
#include <vector>

namespace olouie::audio {
namespace {

AudioCaptureManagerResult Result(AudioCaptureManagerStatus status,
                                 std::wstring message) {
  AudioCaptureManagerResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool HasDuplicateTrackId(const AudioTrackPlan& plan, uint32_t track_id,
                         size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (plan.tracks[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

bool HasDuplicateSource(const std::vector<AudioCaptureSourceBinding>& sources,
                        CapturedAudioSource source) noexcept {
  for (const auto& binding : sources) {
    if (SameCapturedAudioSource(binding.source, source)) {
      return true;
    }
  }
  return false;
}

bool PacketTracksMatchPlan(const AudioTrackPlan& plan) noexcept {
  if (plan.packet_tracks.size() != plan.tracks.size()) {
    return false;
  }

  for (size_t index = 0; index < plan.tracks.size(); ++index) {
    if (plan.packet_tracks[index].track_id != plan.tracks[index].track_id ||
        plan.packet_tracks[index].codec_id != record::CodecId::Aac) {
      return false;
    }
  }

  return true;
}

AudioCaptureSourceRuntime RuntimeFromTrackKind(AudioTrackKind kind) noexcept {
  switch (kind) {
    case AudioTrackKind::SystemLoopback:
      return AudioCaptureSourceRuntime::SystemLoopback;
    case AudioTrackKind::Microphone:
      return AudioCaptureSourceRuntime::Microphone;
    case AudioTrackKind::ProcessLoopback:
      return AudioCaptureSourceRuntime::ProcessLoopback;
    case AudioTrackKind::DefaultMixed:
      break;
  }

  return AudioCaptureSourceRuntime::SystemLoopback;
}

AudioCaptureSourceSupport SupportFromTrackKind(AudioTrackKind kind) noexcept {
  switch (kind) {
    case AudioTrackKind::SystemLoopback:
    case AudioTrackKind::Microphone:
      return AudioCaptureSourceSupport::Supported;
    case AudioTrackKind::ProcessLoopback:
    case AudioTrackKind::DefaultMixed:
      return AudioCaptureSourceSupport::Deferred;
  }

  return AudioCaptureSourceSupport::Deferred;
}

}  // namespace

bool AudioCaptureManagerResult::Succeeded() const noexcept {
  return status == AudioCaptureManagerStatus::Success;
}

AudioCaptureManagerResult AudioCaptureManager::Configure(
    const AudioTrackPlan& plan,
    ICapturedPcmSink* sink) {
  Reset();

  if (sink == nullptr) {
    return Result(AudioCaptureManagerStatus::MissingSink,
                  L"Audio capture manager needs a captured PCM sink.");
  }

  if (!plan.HasTracks() || !PacketTracksMatchPlan(plan)) {
    return Result(AudioCaptureManagerStatus::InvalidPlan,
                  L"Audio capture manager needs a valid audio track plan.");
  }

  bool has_mixed_track = false;
  std::vector<AudioCaptureSourceBinding> built_sources;
  built_sources.reserve(plan.tracks.size());

  for (size_t index = 0; index < plan.tracks.size(); ++index) {
    const auto& track = plan.tracks[index];
    if (track.track_id == 0 ||
        HasDuplicateTrackId(plan, track.track_id, index)) {
      return Result(AudioCaptureManagerStatus::InvalidPlan,
                    L"Audio capture manager needs unique nonzero track ids.");
    }

    if (track.kind == AudioTrackKind::DefaultMixed) {
      has_mixed_track = true;
      continue;
    }

    if (!IsCapturedAudioSourceKind(track.kind)) {
      return Result(AudioCaptureManagerStatus::InvalidPlan,
                    L"Audio capture manager found an unsupported source "
                    L"kind.");
    }

    const CapturedAudioSource source{track.kind, track.source_index};
    if (!IsCapturedAudioSourceValid(source)) {
      return Result(AudioCaptureManagerStatus::InvalidPlan,
                    L"Audio capture manager found an invalid source.");
    }

    if (HasDuplicateSource(built_sources, source)) {
      return Result(AudioCaptureManagerStatus::DuplicateSource,
                    L"Audio capture manager found a duplicate source.");
    }

    AudioCaptureSourceBinding binding;
    binding.source = source;
    binding.runtime = RuntimeFromTrackKind(track.kind);
    binding.support = SupportFromTrackKind(track.kind);
    binding.track_id = track.track_id;
    binding.sink = sink;
    built_sources.push_back(binding);
  }

  if (built_sources.empty()) {
    return Result(AudioCaptureManagerStatus::UnsupportedPlan,
                  L"Audio capture manager cannot start a default mixed-only "
                  L"plan until mixing is implemented.");
  }

  sink_ = sink;
  deferred_mixed_track_ = has_mixed_track;
  sources_ = std::move(built_sources);
  configured_ = true;
  return Result(AudioCaptureManagerStatus::Success, L"");
}

void AudioCaptureManager::Reset() noexcept {
  configured_ = false;
  deferred_mixed_track_ = false;
  sink_ = nullptr;
  sources_.clear();
}

bool AudioCaptureManager::IsConfigured() const noexcept {
  return configured_;
}

size_t AudioCaptureManager::source_count() const noexcept {
  return sources_.size();
}

size_t AudioCaptureManager::supported_source_count() const noexcept {
  size_t count = 0;
  for (const auto& source : sources_) {
    if (source.support == AudioCaptureSourceSupport::Supported) {
      ++count;
    }
  }
  return count;
}

size_t AudioCaptureManager::deferred_source_count() const noexcept {
  size_t count = 0;
  for (const auto& source : sources_) {
    if (source.support == AudioCaptureSourceSupport::Deferred) {
      ++count;
    }
  }
  return count;
}

bool AudioCaptureManager::has_deferred_mixed_track() const noexcept {
  return deferred_mixed_track_;
}

ICapturedPcmSink* AudioCaptureManager::sink() const noexcept {
  return sink_;
}

const std::vector<AudioCaptureSourceBinding>& AudioCaptureManager::sources()
    const noexcept {
  return sources_;
}

const AudioCaptureSourceBinding* AudioCaptureManager::FindSource(
    CapturedAudioSource source) const noexcept {
  for (const auto& binding : sources_) {
    if (SameCapturedAudioSource(binding.source, source)) {
      return &binding;
    }
  }
  return nullptr;
}

bool AudioCaptureSourceIsSupported(
    const AudioCaptureSourceBinding& binding) noexcept {
  return binding.support == AudioCaptureSourceSupport::Supported;
}

const wchar_t* AudioCaptureSourceRuntimeName(
    AudioCaptureSourceRuntime runtime) noexcept {
  switch (runtime) {
    case AudioCaptureSourceRuntime::SystemLoopback:
      return L"System loopback";
    case AudioCaptureSourceRuntime::Microphone:
      return L"Microphone";
    case AudioCaptureSourceRuntime::ProcessLoopback:
      return L"Process loopback";
  }

  return L"Unknown audio capture source";
}

const wchar_t* AudioCaptureSourceSupportName(
    AudioCaptureSourceSupport support) noexcept {
  switch (support) {
    case AudioCaptureSourceSupport::Supported:
      return L"Supported";
    case AudioCaptureSourceSupport::Deferred:
      return L"Deferred";
  }

  return L"Unknown";
}

}  // namespace olouie::audio
