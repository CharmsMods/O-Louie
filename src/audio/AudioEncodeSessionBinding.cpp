#include "audio/AudioEncodeSessionBinding.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioEncodeSessionBindingResult Result(
    AudioEncodeSessionBindingStatus status,
    std::wstring message) {
  AudioEncodeSessionBindingResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool HasDuplicatePlanTrackId(const AudioTrackPlan& plan, uint32_t track_id,
                             size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (plan.tracks[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

bool PlanContainsTrack(const AudioTrackPlan& plan, uint32_t track_id) noexcept {
  for (const auto& track : plan.tracks) {
    if (track.track_id == track_id) {
      return true;
    }
  }
  return false;
}

bool HasDuplicateEncoderSlot(
    std::span<const AudioEncodeSessionEncoderSlot> encoder_slots,
    uint32_t track_id,
    size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (encoder_slots[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

bool HasDuplicateFormatSlot(
    std::span<const AudioEncodeSessionFormatSlot> format_slots,
    uint32_t track_id,
    size_t before_index) noexcept {
  for (size_t index = 0; index < before_index; ++index) {
    if (format_slots[index].track_id == track_id) {
      return true;
    }
  }
  return false;
}

const AudioEncodeSessionEncoderSlot* FindEncoderSlot(
    std::span<const AudioEncodeSessionEncoderSlot> encoder_slots,
    uint32_t track_id) noexcept {
  for (const auto& slot : encoder_slots) {
    if (slot.track_id == track_id) {
      return &slot;
    }
  }
  return nullptr;
}

const AudioEncodeSessionFormatSlot* FindFormatSlot(
    std::span<const AudioEncodeSessionFormatSlot> format_slots,
    uint32_t track_id) noexcept {
  for (const auto& slot : format_slots) {
    if (slot.track_id == track_id) {
      return &slot;
    }
  }
  return nullptr;
}

AudioTrackEncodeChainConfig BuildChainConfig(
    const AudioEncodeSessionBindingOptions& options,
    const AudioEncodeSessionFormatSlot& format_slot) noexcept {
  AudioTrackEncodeChainConfig config;
  config.input_format = format_slot.input_format;
  config.output_sample_rate = format_slot.output_sample_rate == 0
                                  ? format_slot.input_format.sample_rate
                                  : format_slot.output_sample_rate;
  config.queue_capacity = options.queue_capacity;
  config.overflow_policy = options.overflow_policy;
  return config;
}

}  // namespace

bool AudioEncodeSessionBindingResult::Succeeded() const noexcept {
  return status == AudioEncodeSessionBindingStatus::Success;
}

AudioEncodeSessionBindingResult BuildAudioEncodeSessionTracks(
    const AudioTrackPlan& plan,
    const AudioEncodeSessionBindingOptions& options,
    std::span<const AudioEncodeSessionFormatSlot> format_slots,
    std::span<const AudioEncodeSessionEncoderSlot> encoder_slots,
    std::vector<AudioEncodeSessionTrack>* tracks) {
  if (tracks == nullptr) {
    return Result(AudioEncodeSessionBindingStatus::InvalidOptions,
                  L"Audio encode session binding needs an output list.");
  }

  tracks->clear();

  if (!plan.HasTracks()) {
    return Result(AudioEncodeSessionBindingStatus::InvalidPlan,
                  L"Audio encode session binding needs planned audio tracks.");
  }

  if (plan.packet_tracks.size() != plan.tracks.size()) {
    return Result(AudioEncodeSessionBindingStatus::InvalidPlan,
                  L"Audio track plan packet tracks do not match tracks.");
  }

  for (size_t index = 0; index < plan.tracks.size(); ++index) {
    const auto& track = plan.tracks[index];
    const auto& packet_track = plan.packet_tracks[index];
    if (track.track_id == 0 ||
        HasDuplicatePlanTrackId(plan, track.track_id, index)) {
      return Result(AudioEncodeSessionBindingStatus::InvalidPlan,
                    L"Audio track plan has invalid track ids.");
    }

    if (packet_track.track_id != track.track_id ||
        packet_track.codec_id != record::CodecId::Aac) {
      return Result(AudioEncodeSessionBindingStatus::InvalidPlan,
                    L"Audio track plan packet tracks must be matching AAC "
                    L"tracks.");
    }
  }

  if (options.queue_capacity == 0) {
    return Result(AudioEncodeSessionBindingStatus::InvalidOptions,
                  L"Audio encode session binding options are invalid.");
  }

  for (size_t index = 0; index < format_slots.size(); ++index) {
    const auto& slot = format_slots[index];
    if (slot.track_id == 0 || !slot.input_format.IsValid()) {
      return Result(AudioEncodeSessionBindingStatus::MissingFormat,
                    L"Audio encode session binding needs one valid PCM "
                    L"format per planned track.");
    }

    if (HasDuplicateFormatSlot(format_slots, slot.track_id, index)) {
      return Result(AudioEncodeSessionBindingStatus::DuplicateFormat,
                    L"Audio encode session binding has duplicate PCM format "
                    L"slots.");
    }

    if (!PlanContainsTrack(plan, slot.track_id)) {
      return Result(AudioEncodeSessionBindingStatus::UnexpectedFormat,
                    L"Audio encode session binding has a PCM format for an "
                    L"unplanned track.");
    }
  }

  for (size_t index = 0; index < encoder_slots.size(); ++index) {
    const auto& slot = encoder_slots[index];
    if (slot.track_id == 0 || slot.encoder == nullptr) {
      return Result(AudioEncodeSessionBindingStatus::MissingEncoder,
                    L"Audio encode session binding needs one encoder per "
                    L"planned track.");
    }

    if (HasDuplicateEncoderSlot(encoder_slots, slot.track_id, index)) {
      return Result(AudioEncodeSessionBindingStatus::DuplicateEncoder,
                    L"Audio encode session binding has duplicate encoder "
                    L"slots.");
    }

    if (!PlanContainsTrack(plan, slot.track_id)) {
      return Result(AudioEncodeSessionBindingStatus::UnexpectedEncoder,
                    L"Audio encode session binding has an encoder for an "
                    L"unplanned track.");
    }
  }

  std::vector<AudioEncodeSessionTrack> built_tracks;
  built_tracks.reserve(plan.tracks.size());

  for (const auto& track : plan.tracks) {
    const auto* format_slot = FindFormatSlot(format_slots, track.track_id);
    if (format_slot == nullptr) {
      return Result(AudioEncodeSessionBindingStatus::MissingFormat,
                    L"Audio encode session binding is missing a PCM format "
                    L"for a planned track.");
    }

    const AudioTrackEncodeChainConfig chain_config =
        BuildChainConfig(options, *format_slot);
    if (!chain_config.IsValid()) {
      return Result(AudioEncodeSessionBindingStatus::InvalidOptions,
                    L"Audio encode session binding produced invalid track "
                    L"options.");
    }

    const auto* slot = FindEncoderSlot(encoder_slots, track.track_id);
    if (slot == nullptr) {
      return Result(AudioEncodeSessionBindingStatus::MissingEncoder,
                    L"Audio encode session binding is missing an encoder for "
                    L"a planned track.");
    }

    AudioEncodeSessionTrack session_track;
    session_track.track_id = track.track_id;
    session_track.config = chain_config;
    session_track.encoder = slot->encoder;
    built_tracks.push_back(session_track);
  }

  *tracks = std::move(built_tracks);
  return Result(AudioEncodeSessionBindingStatus::Success, L"");
}

}  // namespace olouie::audio
