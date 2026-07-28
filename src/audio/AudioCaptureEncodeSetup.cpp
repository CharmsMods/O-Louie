#include "audio/AudioCaptureEncodeSetup.h"

#include <string>
#include <utility>
#include <vector>

#include "audio/AudioResampler.h"
#include "audio/WasapiLoopbackCapture.h"
#include "audio/WasapiMicCapture.h"

namespace olouie::audio {
namespace {

AudioCaptureEncodeSetupResult Result(
    AudioCaptureEncodeSetupStatus status,
    std::wstring message) {
  AudioCaptureEncodeSetupResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

bool SupportsAacPreparation(const PcmStreamFormat& format) noexcept {
  return (format.encoding == PcmSampleEncoding::Float &&
          format.bits_per_sample == 32) ||
         (format.encoding == PcmSampleEncoding::SignedInteger &&
          format.bits_per_sample == 16);
}

bool DefaultFormatProvider(CapturedAudioSource source,
                           PcmStreamFormat* format,
                           std::wstring* error) {
  switch (source.kind) {
    case AudioTrackKind::SystemLoopback:
      return TryGetDefaultRenderLoopbackFormat(format, error);
    case AudioTrackKind::Microphone:
      return TryGetDefaultMicCaptureFormat(format, error);
    case AudioTrackKind::ProcessLoopback:
    case AudioTrackKind::DefaultMixed:
      break;
  }

  if (error != nullptr) {
    *error = L"No default WASAPI format provider exists for this source.";
  }
  return false;
}

AacEncoderResult DefaultAacEncoderFactory(
    const AudioTrack& track,
    const PcmStreamFormat& prepared_format,
    uint32_t bitrate_bps,
    std::unique_ptr<IAacEncoder>* encoder,
    std::wstring* backend_name,
    AacEncoderOutputMetadata* output_metadata) {
  if (encoder == nullptr) {
    AacEncoderResult result;
    result.status = AacEncoderStatus::InvalidConfig;
    result.message = L"AAC encoder factory needs an output destination.";
    return result;
  }

  auto concrete = std::make_unique<AacEncoder>();
  const auto config = MakeAacEncoderConfig(track, prepared_format, bitrate_bps);
  const auto initialized = concrete->Initialize(config);
  if (!initialized.Succeeded()) {
    return initialized;
  }

  if (backend_name != nullptr) {
    *backend_name = concrete->backend_name();
  }
  if (output_metadata != nullptr) {
    *output_metadata = concrete->output_metadata();
  }
  *encoder = std::move(concrete);
  return initialized;
}

AudioCaptureEncodeSetupResult ValidatePreflightOptions(
    const AudioCaptureEncodePreflightOptions& options) {
  if (options.first_track_id == 0) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode preflight needs nonzero track ids.");
  }

  if (!options.system_loopback && !options.microphone) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode preflight needs at least one "
                  L"requested source.");
  }

  if (!options.separate_source_tracks) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode preflight currently requires "
                  L"separate source tracks.");
  }

  if (options.default_mixed_track) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Default mixed audio setup is deferred.");
  }

  return Result(AudioCaptureEncodeSetupStatus::Success, L"");
}

void AddRequestedSource(CapturedAudioSource source,
                        bool required,
                        std::vector<AudioCaptureEncodeSourcePreflight>* list) {
  AudioCaptureEncodeSourcePreflight entry;
  entry.source = source;
  entry.requested = true;
  entry.required = required;
  list->push_back(std::move(entry));
}

AudioCaptureEncodeSourcePreflight* FindSourcePreflight(
    std::vector<AudioCaptureEncodeSourcePreflight>* sources,
    AudioTrackKind kind,
    uint32_t source_index) noexcept {
  for (auto& source : *sources) {
    if (source.source.kind == kind &&
        source.source.source_index == source_index) {
      return &source;
    }
  }
  return nullptr;
}

const AudioCaptureEncodeTrackPreflight* FindTrackPreflight(
    const AudioCaptureEncodePreflight& preflight,
    uint32_t track_id) noexcept {
  for (const auto& track : preflight.tracks) {
    if (track.track_id == track_id) {
      return &track;
    }
  }
  return nullptr;
}

AudioCaptureEncodeSetupResult QuerySourceFormats(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodeFormatProvider format_provider,
    AudioCaptureEncodePreflight* preflight) {
  for (auto& source : preflight->sources) {
    std::wstring error;
    if (!format_provider(source.source, &source.captured_format, &error)) {
      source.message = std::move(error);
      if (source.required) {
        return Result(AudioCaptureEncodeSetupStatus::SourceUnavailable,
                      source.message);
      }
      continue;
    }

    if (!source.captured_format.IsValid() ||
        !SupportsAacPreparation(source.captured_format)) {
      source.message =
          L"Captured PCM format is not supported by the AAC preparation "
          L"path. Supported capture input is 32-bit float "
          L"PCM or 16-bit signed PCM.";
      if (source.required) {
        return Result(AudioCaptureEncodeSetupStatus::UnsupportedFormat,
                      source.message);
      }
      continue;
    }

    const uint32_t output_sample_rate =
        options.output_sample_rate == 0 ? source.captured_format.sample_rate
                                        : options.output_sample_rate;
    if (output_sample_rate != 44100 && output_sample_rate != 48000) {
      source.message = L"AAC recording output must use 44100 Hz or 48000 Hz.";
      if (source.required) {
        return Result(AudioCaptureEncodeSetupStatus::UnsupportedFormat,
                      source.message);
      }
      continue;
    }

    source.included = true;
  }

  return Result(AudioCaptureEncodeSetupStatus::Success, L"");
}

AudioCaptureEncodeSetupResult BuildPreflightTracks(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodePreflight* preflight) {
  AudioTrackPlanOptions track_options;
  track_options.first_track_id = options.first_track_id;
  track_options.system_loopback = false;
  track_options.microphone = false;
  track_options.process_loopback_count = 0;
  track_options.separate_source_tracks = true;
  track_options.default_mixed_track = false;

  for (const auto& source : preflight->sources) {
    if (!source.included) {
      continue;
    }

    switch (source.source.kind) {
      case AudioTrackKind::SystemLoopback:
        track_options.system_loopback = true;
        break;
      case AudioTrackKind::Microphone:
        track_options.microphone = true;
        break;
      case AudioTrackKind::ProcessLoopback:
      case AudioTrackKind::DefaultMixed:
        return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                      L"Only direct system loopback and microphone tracks "
                      L"are supported by this setup boundary.");
    }
  }

  std::wstring error;
  if (!BuildAudioTrackPlan(track_options, &preflight->plan, &error)) {
    return Result(AudioCaptureEncodeSetupStatus::TrackPlanFailed,
                  std::move(error));
  }

  for (const auto& track : preflight->plan.tracks) {
    auto* source = FindSourcePreflight(&preflight->sources, track.kind,
                                       track.source_index);
    if (source == nullptr || !source->included) {
      return Result(AudioCaptureEncodeSetupStatus::TrackPlanFailed,
                    L"Audio capture encode preflight lost source format "
                    L"metadata for a planned track.");
    }

    const uint32_t output_sample_rate =
        options.output_sample_rate == 0 ? source->captured_format.sample_rate
                                        : options.output_sample_rate;
    const auto prepared_format = MakeSigned16PcmFormat(
        output_sample_rate, source->captured_format.channel_count);
    if (!prepared_format.IsValid()) {
      return Result(AudioCaptureEncodeSetupStatus::UnsupportedFormat,
                    L"Could not build prepared signed-16 PCM format.");
    }

    AudioEncodeSessionFormatSlot slot;
    slot.track_id = track.track_id;
    slot.input_format = source->captured_format;
    slot.output_sample_rate = prepared_format.sample_rate;
    preflight->format_slots.push_back(slot);

    AudioCaptureEncodeTrackPreflight track_preflight;
    track_preflight.track_id = track.track_id;
    track_preflight.kind = track.kind;
    track_preflight.source_index = track.source_index;
    track_preflight.captured_format = source->captured_format;
    track_preflight.prepared_format = prepared_format;
    preflight->tracks.push_back(track_preflight);
  }

  return Result(AudioCaptureEncodeSetupStatus::Success, L"");
}

}  // namespace

bool AudioCaptureEncodeSetupResult::Succeeded() const noexcept {
  return status == AudioCaptureEncodeSetupStatus::Success;
}

void AudioCaptureEncodePreflight::Reset() {
  plan = {};
  sources.clear();
  format_slots.clear();
  tracks.clear();
}

bool AudioCaptureEncodePreflight::IsUsable() const noexcept {
  return plan.HasTracks() && !format_slots.empty() &&
         format_slots.size() == plan.tracks.size() &&
         tracks.size() == plan.tracks.size();
}

void AudioCaptureEncodeSessionSetup::Reset() {
  session.reset();
  session_tracks.clear();
  encoder_slots.clear();
  encoders.clear();
  encoder_infos.clear();
}

bool AudioCaptureEncodeSessionSetup::IsConfigured() const noexcept {
  return session != nullptr && session->IsConfigured();
}

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodePreflight(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodePreflight* preflight) {
  return BuildAudioCaptureEncodePreflight(options, &DefaultFormatProvider,
                                          preflight);
}

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodePreflight(
    const AudioCaptureEncodePreflightOptions& options,
    AudioCaptureEncodeFormatProvider format_provider,
    AudioCaptureEncodePreflight* preflight) {
  if (preflight == nullptr) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode preflight needs an output "
                  L"destination.");
  }

  preflight->Reset();

  if (format_provider == nullptr) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode preflight needs a format provider.");
  }

  const auto validated = ValidatePreflightOptions(options);
  if (!validated.Succeeded()) {
    return validated;
  }

  if (options.system_loopback) {
    AddRequestedSource({AudioTrackKind::SystemLoopback, 0},
                       options.require_system_loopback, &preflight->sources);
  }
  if (options.microphone) {
    AddRequestedSource({AudioTrackKind::Microphone, 0},
                       options.require_microphone, &preflight->sources);
  }

  auto queried = QuerySourceFormats(options, format_provider, preflight);
  if (!queried.Succeeded()) {
    return queried;
  }

  return BuildPreflightTracks(options, preflight);
}

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodeSessionSetup(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetupOptions& options,
    record::PacketStore* packet_store,
    AudioCaptureEncodeSessionSetup* setup) {
  return BuildAudioCaptureEncodeSessionSetup(
      preflight, options, packet_store, &DefaultAacEncoderFactory, setup);
}

AudioCaptureEncodeSetupResult BuildAudioCaptureEncodeSessionSetup(
    const AudioCaptureEncodePreflight& preflight,
    const AudioCaptureEncodeSessionSetupOptions& options,
    record::PacketStore* packet_store,
    AudioCaptureEncodeAacEncoderFactory encoder_factory,
    AudioCaptureEncodeSessionSetup* setup) {
  if (setup == nullptr) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode session setup needs an output "
                  L"destination.");
  }

  setup->Reset();

  if (!preflight.IsUsable()) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode session setup needs a usable "
                  L"preflight.");
  }

  if (options.queue_capacity == 0 || options.aac_bitrate_bps == 0 ||
      packet_store == nullptr || !packet_store->IsWritable()) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode session setup options are invalid.");
  }

  if (encoder_factory == nullptr) {
    return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                  L"Audio capture encode session setup needs an encoder "
                  L"factory.");
  }

  setup->encoders.reserve(preflight.plan.tracks.size());
  setup->encoder_slots.reserve(preflight.plan.tracks.size());
  setup->encoder_infos.reserve(preflight.plan.tracks.size());

  for (const auto& track : preflight.plan.tracks) {
    const auto* track_preflight =
        FindTrackPreflight(preflight, track.track_id);
    if (track_preflight == nullptr ||
        !track_preflight->prepared_format.IsValid()) {
      setup->Reset();
      return Result(AudioCaptureEncodeSetupStatus::InvalidConfig,
                    L"Audio capture encode session setup is missing "
                    L"prepared track format metadata.");
    }

    std::unique_ptr<IAacEncoder> encoder;
    std::wstring backend_name;
    AacEncoderOutputMetadata output_metadata;
    const auto initialized =
        encoder_factory(track, track_preflight->prepared_format,
                        options.aac_bitrate_bps, &encoder, &backend_name,
                        &output_metadata);
    if (!initialized.Succeeded() || encoder == nullptr) {
      setup->Reset();
      std::wstring message =
          L"AAC encoder initialization failed for track " +
          std::to_wstring(track.track_id) + L".";
      if (!initialized.message.empty()) {
        message += L" " + initialized.message;
      }
      return Result(AudioCaptureEncodeSetupStatus::EncoderInitFailed,
                    std::move(message));
    }
    if (backend_name.empty() || !output_metadata.IsReady()) {
      setup->Reset();
      return Result(AudioCaptureEncodeSetupStatus::EncoderInitFailed,
                    L"AAC encoder initialization did not provide complete "
                    L"output metadata for MP4 export.");
    }

    setup->encoder_slots.push_back({track.track_id, encoder.get()});

    AudioCaptureEncodeEncoderInfo info;
    info.track_id = track.track_id;
    info.backend_name = std::move(backend_name);
    info.prepared_format = track_preflight->prepared_format;
    info.output_metadata = std::move(output_metadata);
    setup->encoder_infos.push_back(std::move(info));
    setup->encoders.push_back(std::move(encoder));
  }

  AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = options.queue_capacity;
  binding_options.overflow_policy = options.overflow_policy;

  const auto binding = BuildAudioEncodeSessionTracks(
      preflight.plan, binding_options, preflight.format_slots,
      setup->encoder_slots, &setup->session_tracks);
  if (!binding.Succeeded()) {
    setup->Reset();
    return Result(AudioCaptureEncodeSetupStatus::BindingFailed,
                  binding.message);
  }

  setup->session = std::make_unique<AudioEncodeSession>(
      setup->session_tracks, packet_store);
  if (!setup->session->IsConfigured()) {
    setup->Reset();
    return Result(AudioCaptureEncodeSetupStatus::SessionCreateFailed,
                  L"Audio encode session did not configure.");
  }

  return Result(AudioCaptureEncodeSetupStatus::Success, L"");
}

const wchar_t* AudioCaptureEncodeSetupStatusName(
    AudioCaptureEncodeSetupStatus status) noexcept {
  switch (status) {
    case AudioCaptureEncodeSetupStatus::Success:
      return L"success";
    case AudioCaptureEncodeSetupStatus::InvalidConfig:
      return L"invalid config";
    case AudioCaptureEncodeSetupStatus::SourceUnavailable:
      return L"source unavailable";
    case AudioCaptureEncodeSetupStatus::UnsupportedFormat:
      return L"unsupported format";
    case AudioCaptureEncodeSetupStatus::TrackPlanFailed:
      return L"track plan failed";
    case AudioCaptureEncodeSetupStatus::EncoderInitFailed:
      return L"encoder init failed";
    case AudioCaptureEncodeSetupStatus::BindingFailed:
      return L"binding failed";
    case AudioCaptureEncodeSetupStatus::SessionCreateFailed:
      return L"session create failed";
  }

  return L"unknown";
}

}  // namespace olouie::audio
