#include <objbase.h>
#include <audioclient.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/AacEncoder.h"
#include "audio/AacEncodeSink.h"
#include "audio/AacPacketStore.h"
#include "audio/AudioCaptureEncodeBridge.h"
#include "audio/AudioCaptureEncodeSetup.h"
#include "audio/AudioCaptureEncodeSmoke.h"
#include "audio/AudioCaptureManager.h"
#include "audio/AudioCaptureSmoke.h"
#include "audio/AudioEncodeSessionBinding.h"
#include "audio/AudioEncodeSession.h"
#include "audio/AudioEncodeWorker.h"
#include "audio/AudioEndpointManager.h"
#include "audio/AudioLiveCaptureEncode.h"
#include "audio/AudioRecordingSession.h"
#include "audio/AudioRecordingMetadata.h"
#include "audio/AudioResampler.h"
#include "audio/AudioSourceRouter.h"
#include "audio/AudioSourceSessionDispatcher.h"
#include "audio/AudioTrackEncodeChain.h"
#include "audio/AudioTrackPlan.h"
#include "audio/CapturedPcmSink.h"
#include "audio/CapturedPcmSessionSink.h"
#include "audio/CapturedPcmQueueWriter.h"
#include "audio/MicMonitorSession.h"
#include "audio/PcmAudio.h"
#include "audio/PreparedPcmQueue.h"
#include "audio/WasapiCaptureSource.h"
#include "audio/WasapiLoopbackCapture.h"
#include "audio/WasapiMicCapture.h"
#include "record/PacketStore.h"
#include "record/SessionClock.h"

namespace {

class ComApartment final {
 public:
  bool Initialize() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized_ = SUCCEEDED(result);
    return initialized_;
  }

  ~ComApartment() {
    if (initialized_) {
      CoUninitialize();
    }
  }

 private:
  bool initialized_ = false;
};

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

std::vector<std::byte> BytesFromFloats(const std::vector<float>& samples) {
  const auto bytes = std::as_bytes(
      std::span<const float>(samples.data(), samples.size()));
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> BytesFromSigned16(
    const std::vector<int16_t>& samples) {
  const auto bytes = std::as_bytes(
      std::span<const int16_t>(samples.data(), samples.size()));
  return {bytes.begin(), bytes.end()};
}

int16_t ReadSigned16Sample(const std::vector<std::byte>& bytes,
                           size_t sample_index) {
  int16_t value = 0;
  std::memcpy(&value, bytes.data() + (sample_index * sizeof(value)),
              sizeof(value));
  return value;
}

bool BytesEqual(const std::vector<std::byte>& left,
                const std::vector<std::byte>& right) {
  if (left.size() != right.size()) {
    return false;
  }

  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index]) {
      return false;
    }
  }
  return true;
}

olouie::audio::AacEncoderResult AacResult(
    olouie::audio::AacEncoderStatus status) {
  olouie::audio::AacEncoderResult result;
  result.status = status;
  return result;
}

olouie::audio::AudioEncodeSessionFormatSlot FormatSlot(
    uint32_t track_id,
    olouie::audio::PcmStreamFormat input_format,
    uint32_t output_sample_rate = 0) {
  olouie::audio::AudioEncodeSessionFormatSlot slot;
  slot.track_id = track_id;
  slot.input_format = input_format;
  slot.output_sample_rate = output_sample_rate;
  return slot;
}

std::vector<olouie::audio::AudioEncodeSessionFormatSlot> FormatSlotsForPlan(
    const olouie::audio::AudioTrackPlan& plan,
    olouie::audio::PcmStreamFormat input_format,
    uint32_t output_sample_rate = 0) {
  std::vector<olouie::audio::AudioEncodeSessionFormatSlot> slots;
  slots.reserve(plan.tracks.size());
  for (const auto& track : plan.tracks) {
    slots.push_back(FormatSlot(track.track_id, input_format,
                               output_sample_rate));
  }
  return slots;
}

olouie::audio::CapturedPcmSinkResult CapturedSinkResult(
    olouie::audio::CapturedPcmSinkStatus status) {
  olouie::audio::CapturedPcmSinkResult result;
  result.status = status;
  return result;
}

olouie::audio::PreparedPcmBlock MakePreparedBlock(uint32_t id) {
  olouie::audio::PreparedPcmBlock block;
  block.input.pts_ns = static_cast<int64_t>(id) * 1000000;
  block.input.duration_ns = 1000000;
  block.input.frame_count = 48;
  block.data = {static_cast<std::byte>(id),
                static_cast<std::byte>(id + 1)};
  return block;
}

class FakeAacEncoder final : public olouie::audio::IAacEncoder {
 public:
  olouie::audio::AacEncoderResult SubmitPcm(
      const olouie::audio::AacPcmInput& input,
      std::span<const std::byte> pcm_bytes) override {
    last_input = input;
    last_pcm_size = pcm_bytes.size();
    ++submit_count;
    AddEvent("submit");
    return submit_result;
  }

  olouie::audio::AacEncoderResult DrainAvailable(
      std::vector<olouie::audio::EncodedAacPacket>* packets) override {
    if (packets != nullptr) {
      *packets = drain_packets;
    }
    ++drain_count;
    AddEvent("drain");
    return drain_result;
  }

  olouie::audio::AacEncoderResult Flush(
      std::vector<olouie::audio::EncodedAacPacket>* packets) override {
    if (packets != nullptr) {
      *packets = flush_packets;
    }
    ++flush_count;
    AddEvent("flush");
    return flush_result;
  }

  void AddEvent(std::string_view action) {
    if (events != nullptr) {
      events->push_back(std::string(action) + ":" + event_label);
    }
  }

  olouie::audio::AacEncoderResult submit_result =
      AacResult(olouie::audio::AacEncoderStatus::Success);
  olouie::audio::AacEncoderResult drain_result =
      AacResult(olouie::audio::AacEncoderStatus::Success);
  olouie::audio::AacEncoderResult flush_result =
      AacResult(olouie::audio::AacEncoderStatus::Success);
  std::vector<olouie::audio::EncodedAacPacket> drain_packets;
  std::vector<olouie::audio::EncodedAacPacket> flush_packets;
  olouie::audio::AacPcmInput last_input;
  size_t last_pcm_size = 0;
  uint32_t submit_count = 0;
  uint32_t drain_count = 0;
  uint32_t flush_count = 0;
  std::vector<std::string>* events = nullptr;
  std::string event_label;
};

class FakeCapturedPcmSink final : public olouie::audio::ICapturedPcmSink {
 public:
  olouie::audio::CapturedPcmSinkResult OnCapturedPcm(
      const olouie::audio::CapturedPcmPacket& packet) override {
    last_source = packet.source;
    last_format = packet.format;
    last_packet = packet.packet;
    last_pcm_size = packet.pcm_bytes.size();
    ++packet_count;
    return result;
  }

  olouie::audio::CapturedPcmSinkResult result =
      CapturedSinkResult(olouie::audio::CapturedPcmSinkStatus::Success);
  olouie::audio::CapturedAudioSource last_source;
  olouie::audio::PcmStreamFormat last_format;
  olouie::audio::PcmPacketInfo last_packet;
  size_t last_pcm_size = 0;
  uint32_t packet_count = 0;
};

struct FakeAudioCaptureEncodeSetupState {
  olouie::audio::PcmStreamFormat system_format;
  olouie::audio::PcmStreamFormat mic_format;
  bool system_available = true;
  bool mic_available = true;
  bool encoder_available = true;
  olouie::audio::AacEncoderStatus encoder_failure_status =
      olouie::audio::AacEncoderStatus::BackendUnavailable;
  uint32_t format_query_count = 0;
  uint32_t encoder_create_count = 0;
  std::vector<uint32_t> encoder_track_ids;
  std::vector<olouie::audio::PcmStreamFormat> encoder_input_formats;
  std::vector<uint32_t> encoder_bitrates;
};

FakeAudioCaptureEncodeSetupState g_audio_capture_encode_setup_state;

void ResetFakeAudioCaptureEncodeSetupState() {
  g_audio_capture_encode_setup_state = {};
  g_audio_capture_encode_setup_state.system_format =
      olouie::audio::MakePcmStreamFormat(
          48000, 2, 32, 8, 384000,
          olouie::audio::PcmSampleEncoding::Float);
  g_audio_capture_encode_setup_state.mic_format =
      olouie::audio::MakePcmStreamFormat(
          48000, 1, 16, 2, 96000,
          olouie::audio::PcmSampleEncoding::SignedInteger);
}

bool FakeAudioCaptureEncodeFormatProvider(
    olouie::audio::CapturedAudioSource source,
    olouie::audio::PcmStreamFormat* format,
    std::wstring* error) {
  ++g_audio_capture_encode_setup_state.format_query_count;
  if (format == nullptr) {
    return false;
  }

  if (source.kind == olouie::audio::AudioTrackKind::SystemLoopback) {
    if (!g_audio_capture_encode_setup_state.system_available) {
      if (error != nullptr) {
        *error = L"fake system format unavailable";
      }
      return false;
    }
    *format = g_audio_capture_encode_setup_state.system_format;
    return true;
  }

  if (source.kind == olouie::audio::AudioTrackKind::Microphone) {
    if (!g_audio_capture_encode_setup_state.mic_available) {
      if (error != nullptr) {
        *error = L"fake microphone format unavailable";
      }
      return false;
    }
    *format = g_audio_capture_encode_setup_state.mic_format;
    return true;
  }

  if (error != nullptr) {
    *error = L"fake source unsupported";
  }
  return false;
}

olouie::audio::AacEncoderResult FakeAudioCaptureEncodeEncoderFactory(
    const olouie::audio::AudioTrack& track,
    const olouie::audio::PcmStreamFormat& prepared_format,
    uint32_t bitrate_bps,
    std::unique_ptr<olouie::audio::IAacEncoder>* encoder,
    std::wstring* backend_name,
    olouie::audio::AacEncoderOutputMetadata* output_metadata) {
  ++g_audio_capture_encode_setup_state.encoder_create_count;
  g_audio_capture_encode_setup_state.encoder_track_ids.push_back(
      track.track_id);
  g_audio_capture_encode_setup_state.encoder_input_formats.push_back(
      prepared_format);
  g_audio_capture_encode_setup_state.encoder_bitrates.push_back(bitrate_bps);

  if (!g_audio_capture_encode_setup_state.encoder_available) {
    auto result = AacResult(
        g_audio_capture_encode_setup_state.encoder_failure_status);
    result.message = L"fake AAC encoder unavailable";
    return result;
  }

  if (encoder != nullptr) {
    *encoder = std::make_unique<FakeAacEncoder>();
  }
  if (backend_name != nullptr) {
    *backend_name = L"Fake AAC encoder";
  }
  if (output_metadata != nullptr) {
    output_metadata->sample_rate = prepared_format.sample_rate;
    output_metadata->channel_count = prepared_format.channel_count;
    output_metadata->bitrate_bps = bitrate_bps;
    output_metadata->frame_samples = 1024;
    output_metadata->payload_type = 0;
    output_metadata->profile_level_indication = 0x29;
    output_metadata->audio_object_type = 2;
    output_metadata->audio_specific_config =
        prepared_format.sample_rate == 44100
            ? std::vector<uint8_t>{0x12, static_cast<uint8_t>(
                                            0x08u * prepared_format.channel_count)}
            : std::vector<uint8_t>{0x11, static_cast<uint8_t>(
                                            0x80u + 0x08u * prepared_format.channel_count)};
  }
  return AacResult(olouie::audio::AacEncoderStatus::Success);
}

struct FakeAudioCaptureSmokeState {
  bool system_result = true;
  bool mic_result = true;
  uint32_t system_call_count = 0;
  uint32_t mic_call_count = 0;
  std::chrono::milliseconds last_system_duration{0};
  std::chrono::milliseconds last_mic_duration{0};
};

FakeAudioCaptureSmokeState g_audio_capture_smoke_state;

void ResetFakeAudioCaptureSmokeState() {
  g_audio_capture_smoke_state = {};
}

bool FillFakeSmokePacket(olouie::audio::AudioTrackKind kind,
                         uint64_t device_position,
                         uint64_t qpc_position,
                         olouie::audio::PcmCaptureStats* result,
                         olouie::audio::ICapturedPcmSink* sink,
                         std::wstring* error,
                         bool data_discontinuity = false,
                         bool timestamp_error = false) {
  if (result == nullptr) {
    if (error != nullptr) {
      *error = L"Fake smoke runner needs a result.";
    }
    return false;
  }

  *result = {};
  result->used_event_callback = true;
  result->format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(device_position, qpc_position, 4,
                                           48000, &timing, error)) {
    return false;
  }

  olouie::audio::PcmPacketInfo packet{
      timing, false, data_discontinuity, timestamp_error};
  result->AddPacket(packet);

  const auto pcm_data =
      BytesFromFloats({-1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.75f, -0.25f, 0.25f});
  const auto delivered = olouie::audio::DispatchCapturedPcm(
      sink,
      olouie::audio::CapturedPcmPacket{
          olouie::audio::CapturedAudioSource{kind, 0}, result->format, packet,
          std::span<const std::byte>(pcm_data.data(), pcm_data.size())});
  if (!delivered.Succeeded()) {
    if (error != nullptr) {
      *error = delivered.message;
    }
    return false;
  }

  return true;
}

bool FakeSystemLoopbackSmokeRunner(std::chrono::milliseconds duration,
                                   olouie::audio::PcmCaptureStats* result,
                                   olouie::audio::ICapturedPcmSink* sink,
                                   std::wstring* error) {
  ++g_audio_capture_smoke_state.system_call_count;
  g_audio_capture_smoke_state.last_system_duration = duration;
  if (!g_audio_capture_smoke_state.system_result) {
    if (error != nullptr) {
      *error = L"fake system loopback failure";
    }
    return false;
  }

  return FillFakeSmokePacket(olouie::audio::AudioTrackKind::SystemLoopback,
                             128, 456, result, sink, error);
}

bool FakeMicSmokeRunner(std::chrono::milliseconds duration,
                        olouie::audio::PcmCaptureStats* result,
                        olouie::audio::ICapturedPcmSink* sink,
                        std::wstring* error) {
  ++g_audio_capture_smoke_state.mic_call_count;
  g_audio_capture_smoke_state.last_mic_duration = duration;
  if (!g_audio_capture_smoke_state.mic_result) {
    if (error != nullptr) {
      *error = L"fake microphone failure";
    }
    return false;
  }

  return FillFakeSmokePacket(olouie::audio::AudioTrackKind::Microphone, 256,
                             556, result, sink, error);
}

olouie::audio::AudioCaptureSmokeRunners FakeAudioCaptureSmokeRunners() {
  olouie::audio::AudioCaptureSmokeRunners runners;
  runners.system_loopback = &FakeSystemLoopbackSmokeRunner;
  runners.microphone = &FakeMicSmokeRunner;
  return runners;
}

olouie::audio::WasapiCaptureSourceResult WasapiResult(
    olouie::audio::WasapiCaptureSourceStatus status,
    std::wstring message = L"") {
  olouie::audio::WasapiCaptureSourceResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

struct FakeAudioLiveCaptureState {
  bool system_start_result = true;
  bool mic_start_result = true;
  bool system_final_result = true;
  bool mic_final_result = true;
  bool system_emit_packet = true;
  bool mic_emit_packet = true;
  uint64_t system_qpc_position_100ns = 456;
  uint64_t mic_qpc_position_100ns = 556;
  bool system_data_discontinuity = false;
  bool mic_data_discontinuity = false;
  uint32_t system_start_count = 0;
  uint32_t mic_start_count = 0;
  uint32_t system_stop_count = 0;
  uint32_t mic_stop_count = 0;
  std::vector<std::string> events;
};

FakeAudioLiveCaptureState g_audio_live_capture_state;

void ResetFakeAudioLiveCaptureState() {
  g_audio_live_capture_state = {};
}

class FakeAudioLiveCaptureSource final
    : public olouie::audio::IAudioLiveCaptureSource {
 public:
  explicit FakeAudioLiveCaptureSource(olouie::audio::CapturedAudioSource source)
      : source_(source) {}

  olouie::audio::CapturedAudioSource source() const noexcept override {
    return source_;
  }

  olouie::audio::WasapiCaptureSourceResult Start(
      olouie::audio::ICapturedPcmSink* sink) override {
    if (source_.kind == olouie::audio::AudioTrackKind::SystemLoopback) {
      ++g_audio_live_capture_state.system_start_count;
      g_audio_live_capture_state.events.push_back("start:system");
      if (!g_audio_live_capture_state.system_start_result) {
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::DeviceError,
            L"fake system live capture start failure");
        return last_result_;
      }

      running_ = true;
      std::wstring error;
      if (g_audio_live_capture_state.system_emit_packet &&
          !FillFakeSmokePacket(olouie::audio::AudioTrackKind::SystemLoopback,
                               128,
                               g_audio_live_capture_state
                                   .system_qpc_position_100ns,
                               &stats_, sink, &error,
                               g_audio_live_capture_state
                                   .system_data_discontinuity)) {
        running_ = false;
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::SinkError, error);
        return last_result_;
      }
    } else if (source_.kind == olouie::audio::AudioTrackKind::Microphone) {
      ++g_audio_live_capture_state.mic_start_count;
      g_audio_live_capture_state.events.push_back("start:mic");
      if (!g_audio_live_capture_state.mic_start_result) {
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::DeviceError,
            L"fake microphone live capture start failure");
        return last_result_;
      }

      running_ = true;
      std::wstring error;
      if (g_audio_live_capture_state.mic_emit_packet &&
          !FillFakeSmokePacket(olouie::audio::AudioTrackKind::Microphone, 256,
                               g_audio_live_capture_state
                                   .mic_qpc_position_100ns,
                               &stats_, sink, &error,
                               g_audio_live_capture_state
                                   .mic_data_discontinuity)) {
        running_ = false;
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::SinkError, error);
        return last_result_;
      }
    }

    last_result_ =
        WasapiResult(olouie::audio::WasapiCaptureSourceStatus::Success);
    return last_result_;
  }

  void Stop() override {
    if (source_.kind == olouie::audio::AudioTrackKind::SystemLoopback) {
      ++g_audio_live_capture_state.system_stop_count;
      g_audio_live_capture_state.events.push_back("stop:system");
      if (!g_audio_live_capture_state.system_final_result) {
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::DeviceError,
            L"fake system live capture runtime failure");
      }
    } else if (source_.kind == olouie::audio::AudioTrackKind::Microphone) {
      ++g_audio_live_capture_state.mic_stop_count;
      g_audio_live_capture_state.events.push_back("stop:mic");
      if (!g_audio_live_capture_state.mic_final_result) {
        last_result_ = WasapiResult(
            olouie::audio::WasapiCaptureSourceStatus::DeviceError,
            L"fake microphone live capture runtime failure");
      }
    }
    running_ = false;
  }

  bool IsRunning() const noexcept override {
    return running_;
  }

  olouie::audio::PcmCaptureStats SnapshotStats() const override {
    return stats_;
  }

  olouie::audio::WasapiCaptureSourceResult LastResult() const override {
    return last_result_;
  }

 private:
  olouie::audio::CapturedAudioSource source_;
  bool running_ = false;
  olouie::audio::PcmCaptureStats stats_;
  olouie::audio::WasapiCaptureSourceResult last_result_ =
      WasapiResult(olouie::audio::WasapiCaptureSourceStatus::Success);
};

std::unique_ptr<olouie::audio::IAudioLiveCaptureSource>
FakeAudioLiveCaptureSourceFactory(
    const olouie::audio::AudioCaptureSourceBinding& binding) {
  return std::make_unique<FakeAudioLiveCaptureSource>(binding.source);
}

int VerifyFlow(olouie::audio::AudioEndpointFlow flow) {
  std::wstring error;
  std::vector<olouie::audio::AudioEndpointInfo> endpoints;
  if (!olouie::audio::EnumerateActiveAudioEndpoints(flow, &endpoints, &error)) {
    std::wcerr << L"Endpoint enumeration failed: " << error << L'\n';
    return 1;
  }

  for (const auto& endpoint : endpoints) {
    if (endpoint.id.empty() || endpoint.name.empty() || endpoint.flow != flow) {
      return Fail("Endpoint metadata is incomplete.");
    }
  }

  olouie::audio::AudioEndpointInfo default_endpoint;
  if (olouie::audio::TryGetDefaultAudioEndpoint(flow, &default_endpoint,
                                                &error)) {
    bool found_default = false;
    for (const auto& endpoint : endpoints) {
      if (endpoint.id == default_endpoint.id && endpoint.is_default) {
        found_default = true;
        break;
      }
    }

    if (!found_default) {
      return Fail("Default endpoint was not reflected in enumeration.");
    }
  }

  return 0;
}

int VerifyPcmAudioModel() {
  const auto format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  if (!format.IsValid()) {
    return Fail("Expected PCM stream format to be valid.");
  }

  if (olouie::audio::AudioFramesToNs(48000, 48000) != 1000000000 ||
      olouie::audio::Qpc100nsToNs(123) != 12300) {
    return Fail("PCM timing conversion changed unexpectedly.");
  }

  olouie::audio::PcmPacketTiming timing;
  std::wstring error;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 480, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM packet timing build failed: " << error << L'\n';
    return 1;
  }

  if (!timing.IsValid() || timing.device_position_frames != 128 ||
      timing.qpc_position_100ns != 456 || timing.qpc_position_ns != 45600 ||
      timing.duration_ns != 10000000) {
    return Fail("PCM packet timing fields are incorrect.");
  }

  olouie::audio::PcmCaptureStats stats;
  stats.format = format;
  stats.used_event_callback = true;
  stats.AddPacket({timing, false});
  olouie::audio::PcmPacketTiming second;
  if (!olouie::audio::BuildPcmPacketTiming(608, 556, 480, 48000, &second,
                                           &error)) {
    return 1;
  }
  stats.AddPacket({second, true, true, true});

  if (!stats.HasPackets() || stats.packet_count != 2 ||
      stats.frame_count != 960 || stats.silent_packet_count != 1 ||
      stats.data_discontinuity_count != 1 ||
      stats.timestamp_error_count != 1 ||
      stats.first_packet.device_position_frames != 128 ||
      stats.last_packet.device_position_frames != 608) {
    return Fail("PCM capture stats accumulation is incorrect.");
  }

  if (olouie::audio::BuildPcmPacketTiming(0, 0, 0, 48000, &timing, &error)) {
    return Fail("PCM packet timing should reject zero-frame packets.");
  }

  return 0;
}

int VerifyCapturedPcmSinkBoundary() {
  const auto format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::PcmPacketTiming timing;
  std::wstring error;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for captured sink failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  const auto pcm_data = BytesFromFloats(float_samples);

  olouie::audio::CapturedPcmPacket captured{
      olouie::audio::CapturedAudioSource{
          olouie::audio::AudioTrackKind::SystemLoopback, 0},
      format, packet,
      std::span<const std::byte>(pcm_data.data(), pcm_data.size())};
  if (!captured.IsValid()) {
    return Fail("Captured PCM packet should be valid.");
  }

  auto delivered = olouie::audio::DispatchCapturedPcm(nullptr, captured);
  if (!delivered.Succeeded()) {
    return Fail("Captured PCM dispatch should allow a null sink.");
  }

  FakeCapturedPcmSink sink;
  delivered = olouie::audio::DispatchCapturedPcm(&sink, captured);
  if (!delivered.Succeeded() || sink.packet_count != 1 ||
      sink.last_source.kind != olouie::audio::AudioTrackKind::SystemLoopback ||
      sink.last_source.source_index != 0 ||
      sink.last_format.sample_rate != 48000 ||
      sink.last_packet.timing.frame_count != 4 ||
      sink.last_pcm_size != pcm_data.size()) {
    return Fail("Captured PCM dispatch did not call the sink correctly.");
  }

  olouie::audio::CapturedPcmPacket silent{
      olouie::audio::CapturedAudioSource{
          olouie::audio::AudioTrackKind::Microphone, 0},
      format, olouie::audio::PcmPacketInfo{timing, true},
      std::span<const std::byte>()};
  if (!silent.IsValid()) {
    return Fail("Captured PCM silent packet should allow empty PCM bytes.");
  }

  delivered = olouie::audio::DispatchCapturedPcm(&sink, silent);
  if (!delivered.Succeeded() || sink.packet_count != 2 ||
      sink.last_source.kind != olouie::audio::AudioTrackKind::Microphone ||
      !sink.last_packet.silent || sink.last_pcm_size != 0) {
    return Fail("Captured PCM dispatch did not handle silent packets.");
  }

  olouie::audio::CapturedPcmPacket invalid_source = captured;
  invalid_source.source.kind = olouie::audio::AudioTrackKind::DefaultMixed;
  if (invalid_source.IsValid() ||
      olouie::audio::DispatchCapturedPcm(&sink, invalid_source).status !=
          olouie::audio::CapturedPcmSinkStatus::InvalidPacket) {
    return Fail("Captured PCM sink should reject mixer-output sources.");
  }

  olouie::audio::CapturedPcmPacket invalid_bytes = captured;
  invalid_bytes.pcm_bytes = std::span<const std::byte>();
  if (invalid_bytes.IsValid() ||
      olouie::audio::DispatchCapturedPcm(&sink, invalid_bytes).status !=
          olouie::audio::CapturedPcmSinkStatus::InvalidPacket) {
    return Fail("Captured PCM sink should reject missing non-silent bytes.");
  }

  FakeCapturedPcmSink failing_sink;
  failing_sink.result =
      CapturedSinkResult(olouie::audio::CapturedPcmSinkStatus::SinkError);
  delivered = olouie::audio::DispatchCapturedPcm(&failing_sink, captured);
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::SinkError ||
      failing_sink.packet_count != 1) {
    return Fail("Captured PCM dispatch should surface sink failures.");
  }

  return 0;
}

int VerifyAudioTrackPlan() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;

  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Default audio track plan failed: " << error << L'\n';
    return 1;
  }

  if (!plan.HasTracks() || plan.HasDefaultMixedTrack() ||
      plan.tracks.size() != 1 || plan.packet_tracks.size() != 1 ||
      plan.tracks[0].track_id != 2 ||
      plan.tracks[0].kind != olouie::audio::AudioTrackKind::SystemLoopback ||
      plan.packet_tracks[0].codec_id != olouie::record::CodecId::Aac) {
    return Fail("Default audio track plan shape is incorrect.");
  }

  options.first_track_id = 10;
  options.microphone = true;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Mixed audio track plan failed: " << error << L'\n';
    return 1;
  }

  if (!plan.HasDefaultMixedTrack() || plan.tracks.size() != 3 ||
      plan.tracks[0].track_id != 10 ||
      plan.tracks[0].kind != olouie::audio::AudioTrackKind::DefaultMixed ||
      plan.tracks[1].track_id != 11 ||
      plan.tracks[1].kind != olouie::audio::AudioTrackKind::SystemLoopback ||
      plan.tracks[2].track_id != 12 ||
      plan.tracks[2].kind != olouie::audio::AudioTrackKind::Microphone) {
    return Fail("Mixed plus separate audio track ordering is incorrect.");
  }

  options = {};
  options.first_track_id = 20;
  options.microphone = true;
  options.separate_source_tracks = false;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error) ||
      plan.tracks.size() != 1 ||
      plan.tracks[0].kind != olouie::audio::AudioTrackKind::DefaultMixed ||
      plan.tracks[0].track_id != 20) {
    return Fail("Mixed-only audio track plan is incorrect.");
  }

  options = {};
  options.first_track_id = 4;
  options.system_loopback = false;
  options.default_mixed_track = false;
  options.process_loopback_count = 2;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error) ||
      plan.tracks.size() != 2 ||
      plan.tracks[0].kind != olouie::audio::AudioTrackKind::ProcessLoopback ||
      plan.tracks[0].source_index != 0 || plan.tracks[0].track_id != 4 ||
      plan.tracks[1].source_index != 1 || plan.tracks[1].track_id != 5) {
    return Fail("Process-loopback audio track planning is incorrect.");
  }

  options = {};
  options.system_loopback = false;
  options.default_mixed_track = false;
  if (olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    return Fail("Audio planning should reject configs without sources.");
  }

  options = {};
  options.separate_source_tracks = false;
  options.default_mixed_track = false;
  if (olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    return Fail("Audio planning should reject configs without emitted tracks.");
  }

  options = {};
  options.first_track_id = std::numeric_limits<uint32_t>::max();
  options.microphone = true;
  if (olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    return Fail("Audio planning should reject overflowing track ids.");
  }

  return 0;
}

int VerifyAudioSourceRouterBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.first_track_id = 20;
  options.microphone = true;
  options.process_loopback_count = 2;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for source router failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 5) {
    return Fail("Audio source router test expected mixed plus four sources.");
  }

  olouie::audio::AudioSourceRouter router(plan);
  if (!router.IsConfigured() || router.route_count() != 4) {
    return Fail("Audio source router did not configure source routes.");
  }

  auto route = router.ResolveTrack(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0});
  if (!route.Succeeded() || route.track_id != 21) {
    return Fail("Audio source router did not resolve system loopback.");
  }

  route =
      router.ResolveTrack({olouie::audio::AudioTrackKind::Microphone, 0});
  if (!route.Succeeded() || route.track_id != 22) {
    return Fail("Audio source router did not resolve microphone.");
  }

  route = router.ResolveTrack(
      {olouie::audio::AudioTrackKind::ProcessLoopback, 0});
  if (!route.Succeeded() || route.track_id != 23) {
    return Fail("Audio source router did not resolve process source 0.");
  }

  route = router.ResolveTrack(
      {olouie::audio::AudioTrackKind::ProcessLoopback, 1});
  if (!route.Succeeded() || route.track_id != 24) {
    return Fail("Audio source router did not resolve process source 1.");
  }

  route = router.ResolveTrack(
      {olouie::audio::AudioTrackKind::ProcessLoopback, 2});
  if (route.status != olouie::audio::AudioSourceRouteStatus::SourceNotEnabled ||
      route.track_id != 0) {
    return Fail("Audio source router should reject disabled process sources.");
  }

  route =
      router.ResolveTrack({olouie::audio::AudioTrackKind::Microphone, 1});
  if (route.status != olouie::audio::AudioSourceRouteStatus::InvalidSource) {
    return Fail("Audio source router should reject indexed microphones.");
  }

  route = router.ResolveTrack(
      {olouie::audio::AudioTrackKind::DefaultMixed, 0});
  if (route.status != olouie::audio::AudioSourceRouteStatus::InvalidSource) {
    return Fail("Audio source router should reject default mixed as a source.");
  }

  options = {};
  options.first_track_id = 30;
  options.microphone = true;
  options.separate_source_tracks = false;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Mixed-only plan for source router failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::AudioSourceRouter mixed_only_router(plan);
  if (!mixed_only_router.IsConfigured() ||
      mixed_only_router.route_count() != 0) {
    return Fail("Mixed-only audio plan should configure no direct routes.");
  }

  route = mixed_only_router.ResolveTrack(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0});
  if (route.status != olouie::audio::AudioSourceRouteStatus::SourceNotEnabled) {
    return Fail("Mixed-only audio plan should not direct-route sources.");
  }

  olouie::audio::AudioTrackPlan empty_plan;
  olouie::audio::AudioSourceRouter empty_router(empty_plan);
  if (empty_router.IsConfigured() ||
      empty_router.ResolveTrack(
                      {olouie::audio::AudioTrackKind::SystemLoopback, 0})
              .status != olouie::audio::AudioSourceRouteStatus::InvalidPlan) {
    return Fail("Audio source router should reject empty plans.");
  }

  options = {};
  options.first_track_id = 40;
  options.default_mixed_track = false;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Single-source plan for source router failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::AudioTrackPlan malformed_plan = plan;
  malformed_plan.packet_tracks[0].track_id = 9999;
  olouie::audio::AudioSourceRouter malformed_router(malformed_plan);
  if (malformed_router.IsConfigured()) {
    return Fail("Audio source router should reject malformed packet tracks.");
  }

  olouie::audio::AudioTrackPlan duplicate_track_plan = plan;
  duplicate_track_plan.tracks.push_back(duplicate_track_plan.tracks[0]);
  duplicate_track_plan.tracks.back().track_id = 41;
  duplicate_track_plan.packet_tracks.push_back(
      duplicate_track_plan.tracks.back().ToPacketTrack());
  olouie::audio::AudioSourceRouter duplicate_track_router(
      duplicate_track_plan);
  if (duplicate_track_router.IsConfigured()) {
    return Fail("Audio source router should reject duplicate source routes.");
  }

  olouie::audio::AudioTrackPlan duplicate_id_plan = plan;
  duplicate_id_plan.tracks.push_back(duplicate_id_plan.tracks[0]);
  duplicate_id_plan.packet_tracks.push_back(
      duplicate_id_plan.tracks.back().ToPacketTrack());
  olouie::audio::AudioSourceRouter duplicate_id_router(duplicate_id_plan);
  if (duplicate_id_router.IsConfigured()) {
    return Fail("Audio source router should reject duplicate track ids.");
  }

  olouie::audio::AudioTrackPlan invalid_source_plan = plan;
  invalid_source_plan.tracks[0].source_index = 1;
  invalid_source_plan.packet_tracks[0] =
      invalid_source_plan.tracks[0].ToPacketTrack();
  olouie::audio::AudioSourceRouter invalid_source_router(invalid_source_plan);
  if (invalid_source_router.IsConfigured()) {
    return Fail("Audio source router should reject invalid source metadata.");
  }

  return 0;
}

int VerifyAacEncoderBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for AAC test failed: " << error << L'\n';
    return 1;
  }

  const auto s16_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 16, 4, 192000,
      olouie::audio::PcmSampleEncoding::SignedInteger);
  auto config =
      olouie::audio::MakeAacEncoderConfig(plan.tracks[0], s16_format, 192000);

  const auto validation = olouie::audio::ValidateAacEncoderConfig(config);
  if (!validation.Succeeded()) {
    std::wcerr << L"Valid AAC config rejected: " << validation.message << L'\n';
    return 1;
  }

  if (config.packet_track.track_id != plan.tracks[0].track_id ||
      config.packet_track.codec_id != olouie::record::CodecId::Aac ||
      config.output_sample_rate != 48000 ||
      config.output_channel_count != 2 ||
      config.aac_frame_samples != 1024) {
    return Fail("AAC config construction is incorrect.");
  }

  olouie::audio::AacEncoderOutputMetadata output_metadata;
  output_metadata.sample_rate = 48000;
  output_metadata.channel_count = 2;
  output_metadata.bitrate_bps = 192000;
  output_metadata.frame_samples = 1024;
  output_metadata.payload_type = 0;
  output_metadata.profile_level_indication = 0x29;
  output_metadata.audio_object_type = 2;
  output_metadata.audio_specific_config = {0x11, 0x90};
  if (!output_metadata.IsReady()) {
    return Fail("Complete AAC output metadata should be ready for MP4.");
  }
  output_metadata.payload_type = 1;
  if (output_metadata.IsReady()) {
    return Fail("ADTS AAC metadata should not enter the raw MP4 path.");
  }

  olouie::audio::EncodedAacPacket packet;
  if (packet.IsValid()) {
    return Fail("Empty AAC packet should not be valid.");
  }
  packet.track_id = config.track.track_id;
  packet.duration_ns = 1000000;
  packet.data.push_back(std::byte{0x7f});
  if (!packet.IsValid()) {
    return Fail("Populated AAC packet should be valid.");
  }

  olouie::audio::AacEncoder uninitialized_encoder;
  std::vector<olouie::audio::EncodedAacPacket> packets;
  const olouie::audio::AacPcmInput input{
      0, olouie::audio::AudioFramesToNs(1024, 48000), 1024};
  std::vector<std::byte> silence(1024 * s16_format.block_align,
                                 std::byte{0});
  if (uninitialized_encoder.SubmitPcm(input, silence).status !=
          olouie::audio::AacEncoderStatus::InvalidState ||
      uninitialized_encoder.DrainAvailable(&packets).status !=
          olouie::audio::AacEncoderStatus::InvalidState ||
      uninitialized_encoder.Flush(&packets).status !=
          olouie::audio::AacEncoderStatus::InvalidState) {
    return Fail("AAC encoder should reject packet calls before initialization.");
  }

  auto bad_config = config;
  bad_config.packet_track.track_id = config.track.track_id + 1;
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject mismatched packet track ids.");
  }

  bad_config = config;
  bad_config.packet_track.codec_id = olouie::record::CodecId::H264;
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject non-AAC packet tracks.");
  }

  const auto float_capture_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  bad_config =
      olouie::audio::MakeAacEncoderConfig(plan.tracks[0], float_capture_format,
                                          192000);
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject unconverted float PCM.");
  }

  bad_config = config;
  bad_config.input_format.block_align = 2;
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject inconsistent PCM layout.");
  }

  bad_config = config;
  bad_config.output_sample_rate = 44100;
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject sample-rate mismatch.");
  }

  bad_config = config;
  bad_config.aac_frame_samples = 960;
  if (olouie::audio::ValidateAacEncoderConfig(bad_config).Succeeded()) {
    return Fail("AAC config should reject non-AAC-LC frame sample count.");
  }

  return 0;
}

int VerifyAacPacketStoreHandoff() {
  olouie::audio::EncodedAacPacket packet;
  packet.track_id = 7;
  packet.pts_ns = 20000000;
  packet.dts_ns = 20000000;
  packet.duration_ns = 21333333;
  packet.data = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

  olouie::record::PacketMetadata metadata;
  std::wstring error;
  if (!olouie::audio::BuildAacPacketMetadata(packet, &metadata, &error)) {
    std::wcerr << L"AAC metadata build failed: " << error << L'\n';
    return 1;
  }

  if (metadata.track_id != packet.track_id ||
      metadata.codec_id != olouie::record::CodecId::Aac ||
      metadata.flags != olouie::record::PacketFlagNone ||
      metadata.pts_ns != packet.pts_ns ||
      metadata.dts_ns != packet.dts_ns ||
      metadata.duration_ns != packet.duration_ns) {
    return Fail("AAC packet metadata mapping is incorrect.");
  }

  olouie::audio::EncodedAacPacket invalid_packet;
  if (olouie::audio::BuildAacPacketMetadata(invalid_packet, &metadata,
                                            &error)) {
    return Fail("AAC metadata build should reject invalid packets.");
  }

  if (olouie::audio::AppendAacPacket(nullptr, packet, &error)) {
    return Fail("AAC packet append should reject null PacketStore.");
  }
  error.clear();

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioPacketTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  const std::array tracks{
      olouie::record::TrackDefinition{7, olouie::record::CodecId::Aac},
  };
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for AAC failed: " << error << L'\n';
    return 1;
  }

  if (!olouie::audio::AppendAacPacket(&store, packet, &error)) {
    std::wcerr << L"AAC packet append failed: " << error << L'\n';
    return 1;
  }

  const auto index = store.SnapshotIndex();
  if (index.size() != 1 ||
      index[0].metadata.codec_id != olouie::record::CodecId::Aac ||
      index[0].metadata.track_id != packet.track_id ||
      index[0].payload_size != packet.data.size()) {
    return Fail("AAC PacketStore index entry is incorrect.");
  }

  const auto range = store.QueryRange(packet.pts_ns,
                                      packet.pts_ns + packet.duration_ns,
                                      false);
  if (range.packets.size() != 1 ||
      range.packets[0].metadata.track_id != packet.track_id) {
    return Fail("AAC PacketStore range query is incorrect.");
  }

  std::vector<std::byte> payload;
  if (!store.ReadPayload(range.packets[0], &payload, &error) ||
      !BytesEqual(payload, packet.data)) {
    std::wcerr << L"AAC payload readback failed: " << error << L'\n';
    return 1;
  }

  auto wrong_track_packet = packet;
  wrong_track_packet.track_id = 8;
  if (olouie::audio::AppendAacPacket(&store, wrong_track_packet, &error)) {
    return Fail("AAC packet append should reject unknown PacketStore track ids.");
  }

  store.Close();
  auto recovered = olouie::record::PacketStore::Recover(session_dir, &error);
  const auto recovered_index = recovered.SnapshotIndex();
  if (recovered_index.size() != 1 ||
      recovered_index[0].metadata.track_id != packet.track_id ||
      recovered_index[0].metadata.codec_id != olouie::record::CodecId::Aac) {
    return Fail("Recovered AAC PacketStore index is incorrect.");
  }

  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAacEncodeSinkBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for sink test failed: " << error << L'\n';
    return 1;
  }

  const auto s16_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 16, 4, 192000,
      olouie::audio::PcmSampleEncoding::SignedInteger);
  const auto config =
      olouie::audio::MakeAacEncoderConfig(plan.tracks[0], s16_format, 192000);

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioSinkTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for sink failed: " << error << L'\n';
    return 1;
  }

  const olouie::audio::AacPcmInput input{
      0, olouie::audio::AudioFramesToNs(1024, 48000), 1024};
  std::vector<std::byte> silence(1024 * s16_format.block_align,
                                 std::byte{0});

  olouie::audio::AacEncoder uninitialized_encoder;
  olouie::audio::AacEncodeSink null_encoder_sink(nullptr, &store);
  if (null_encoder_sink.SubmitPreparedPcm(input, silence).status !=
      olouie::audio::AacEncodeSinkStatus::InvalidArgument) {
    return Fail("AAC encode sink should reject a null encoder.");
  }

  olouie::audio::AacEncodeSink null_store_sink(&uninitialized_encoder, nullptr);
  if (null_store_sink.DrainAvailable().status !=
      olouie::audio::AacEncodeSinkStatus::InvalidArgument) {
    return Fail("AAC encode sink should reject a null PacketStore.");
  }

  olouie::audio::AacEncodeSink uninitialized_sink(&uninitialized_encoder,
                                                  &store);
  if (uninitialized_sink.SubmitPreparedPcm(input, silence).status !=
      olouie::audio::AacEncodeSinkStatus::EncoderError) {
    return Fail("AAC encode sink should surface uninitialized encoder errors.");
  }

  FakeAacEncoder fake_encoder;
  olouie::audio::EncodedAacPacket drain_packet;
  drain_packet.track_id = plan.tracks[0].track_id;
  drain_packet.pts_ns = 0;
  drain_packet.dts_ns = 0;
  drain_packet.duration_ns = input.duration_ns;
  drain_packet.data = {std::byte{0x41}, std::byte{0x42}};

  olouie::audio::EncodedAacPacket flush_packet;
  flush_packet.track_id = plan.tracks[0].track_id;
  flush_packet.pts_ns = input.duration_ns;
  flush_packet.dts_ns = input.duration_ns;
  flush_packet.duration_ns = input.duration_ns;
  flush_packet.data = {std::byte{0x43}, std::byte{0x44}};

  fake_encoder.drain_packets = {drain_packet};
  fake_encoder.flush_packets = {flush_packet};

  olouie::audio::AacEncodeSink sink(&fake_encoder, &store);
  const auto submit_result = sink.SubmitPreparedPcm(input, silence);
  if (!submit_result.Succeeded()) {
    std::wcerr << L"AAC sink submit failed: " << submit_result.message
               << L'\n';
    return 1;
  }

  if (fake_encoder.submit_count != 1 || fake_encoder.drain_count != 1 ||
      fake_encoder.last_input.frame_count != input.frame_count ||
      fake_encoder.last_pcm_size != silence.size()) {
    return Fail("AAC encode sink did not submit and drain as expected.");
  }

  const auto flush_result = sink.Flush();
  if (!flush_result.Succeeded() || fake_encoder.flush_count != 1) {
    std::wcerr << L"AAC sink flush failed: " << flush_result.message << L'\n';
    return 1;
  }

  const auto stats = sink.stats();
  if (stats.submitted_block_count != 1 ||
      stats.submitted_frame_count != input.frame_count ||
      stats.drained_packet_count != 2 ||
      stats.appended_packet_count != 2) {
    return Fail("AAC encode sink stats are inconsistent.");
  }

  const auto range = store.QueryRange(0, input.duration_ns * 3, false);
  if (range.packets.size() != 2) {
    return Fail("AAC encode sink PacketStore range query is inconsistent.");
  }

  for (const auto& entry : range.packets) {
    if (entry.metadata.codec_id != olouie::record::CodecId::Aac ||
        entry.metadata.track_id != plan.tracks[0].track_id ||
        entry.payload_size == 0) {
      return Fail("AAC encode sink wrote invalid PacketStore entries.");
    }
  }

  fake_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  if (sink.SubmitPreparedPcm(input, silence).status !=
      olouie::audio::AacEncodeSinkStatus::EncoderError) {
    return Fail("AAC encode sink should surface submit failures.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyPreparedPcmQueueBoundary() {
  olouie::audio::PreparedPcmQueue zero_capacity(0);
  if (zero_capacity.TryPush(MakePreparedBlock(1)).status !=
      olouie::audio::PreparedPcmQueueStatus::InvalidConfig) {
    return Fail("Prepared PCM queue should reject zero capacity.");
  }

  auto stats = zero_capacity.SnapshotStats();
  if (stats.rejected_block_count != 1 || zero_capacity.size() != 0) {
    return Fail("Zero-capacity queue stats are incorrect.");
  }

  olouie::audio::PreparedPcmQueue reject_queue(2);
  olouie::audio::PreparedPcmBlock invalid_block;
  if (reject_queue.TryPush(invalid_block).status !=
      olouie::audio::PreparedPcmQueueStatus::InvalidBlock) {
    return Fail("Prepared PCM queue should reject invalid blocks.");
  }

  if (!reject_queue.TryPush(MakePreparedBlock(10)).Succeeded() ||
      !reject_queue.TryPush(MakePreparedBlock(20)).Succeeded() ||
      reject_queue.TryPush(MakePreparedBlock(30)).status !=
          olouie::audio::PreparedPcmQueueStatus::QueueFull ||
      !reject_queue.full() || reject_queue.size() != 2) {
    return Fail("Prepared PCM reject-newest queue behavior is incorrect.");
  }

  stats = reject_queue.SnapshotStats();
  if (stats.pushed_block_count != 2 || stats.rejected_block_count != 2 ||
      stats.dropped_block_count != 0) {
    return Fail("Prepared PCM reject-newest stats are incorrect.");
  }

  olouie::audio::PreparedPcmBlock popped;
  if (!reject_queue.TryPop(&popped) || popped.input.pts_ns != 10000000 ||
      !reject_queue.TryPop(&popped) || popped.input.pts_ns != 20000000 ||
      reject_queue.TryPop(&popped) || !reject_queue.empty()) {
    return Fail("Prepared PCM reject-newest FIFO order is incorrect.");
  }

  stats = reject_queue.SnapshotStats();
  if (stats.popped_block_count != 2) {
    return Fail("Prepared PCM pop stats are incorrect.");
  }

  olouie::audio::PreparedPcmQueue drop_queue(
      2, olouie::audio::PreparedPcmOverflowPolicy::DropOldest);
  if (!drop_queue.TryPush(MakePreparedBlock(1)).Succeeded() ||
      !drop_queue.TryPush(MakePreparedBlock(2)).Succeeded() ||
      !drop_queue.TryPush(MakePreparedBlock(3)).Succeeded() ||
      drop_queue.size() != 2) {
    return Fail("Prepared PCM drop-oldest push behavior is incorrect.");
  }

  stats = drop_queue.SnapshotStats();
  if (stats.pushed_block_count != 3 || stats.dropped_block_count != 1 ||
      stats.rejected_block_count != 0) {
    return Fail("Prepared PCM drop-oldest stats are incorrect.");
  }

  if (!drop_queue.TryPop(&popped) || popped.input.pts_ns != 2000000 ||
      !drop_queue.TryPop(&popped) || popped.input.pts_ns != 3000000) {
    return Fail("Prepared PCM drop-oldest retained the wrong blocks.");
  }

  const auto block = MakePreparedBlock(7);
  if (!drop_queue.TryPush(block.input, block.data).Succeeded() ||
      !drop_queue.TryPop(&popped) || popped.input.pts_ns != block.input.pts_ns ||
      !BytesEqual(popped.data, block.data)) {
    return Fail("Prepared PCM queue span-push copy is incorrect.");
  }

  drop_queue.Clear();
  if (!drop_queue.empty() || drop_queue.capacity() != 2) {
    return Fail("Prepared PCM queue clear behavior is incorrect.");
  }

  return 0;
}

int VerifyAudioEncodeWorkerBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for worker test failed: " << error
               << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioWorkerTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for worker failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PreparedPcmQueue queue(4);
  FakeAacEncoder fake_encoder;
  olouie::audio::EncodedAacPacket packet;
  packet.track_id = plan.tracks[0].track_id;
  packet.pts_ns = 0;
  packet.dts_ns = 0;
  packet.duration_ns = 1000000;
  packet.data = {std::byte{0x51}, std::byte{0x52}};
  fake_encoder.drain_packets = {packet};

  olouie::audio::AacEncodeSink sink(&fake_encoder, &store);
  olouie::audio::AudioEncodeWorker invalid_worker(nullptr, &sink);
  if (invalid_worker.DrainQueuedBlocks(1).status !=
      olouie::audio::AudioEncodeWorkerStatus::InvalidArgument) {
    return Fail("Audio encode worker should reject missing dependencies.");
  }

  olouie::audio::AudioEncodeWorker worker(&queue, &sink);
  if (!queue.TryPush(MakePreparedBlock(1)).Succeeded() ||
      !queue.TryPush(MakePreparedBlock(2)).Succeeded() ||
      !queue.TryPush(MakePreparedBlock(3)).Succeeded()) {
    return Fail("Audio encode worker test queue setup failed.");
  }

  auto result = worker.DrainQueuedBlocks(2);
  if (!result.Succeeded() || result.submitted_block_count != 2 ||
      fake_encoder.submit_count != 2 || queue.size() != 1) {
    return Fail("Audio encode worker did not drain the requested block budget.");
  }

  result = worker.DrainAllQueuedBlocks();
  if (!result.Succeeded() || result.submitted_block_count != 1 ||
      fake_encoder.submit_count != 3 || !queue.empty()) {
    return Fail("Audio encode worker did not drain the remaining block.");
  }

  const auto stats = worker.stats();
  if (stats.submitted_block_count != 3 ||
      stats.submitted_frame_count != 144 || stats.sink_error_count != 0) {
    return Fail("Audio encode worker stats are incorrect.");
  }

  const auto sink_stats = sink.stats();
  if (sink_stats.submitted_block_count != 3 ||
      sink_stats.appended_packet_count != 3) {
    return Fail("Audio encode worker did not hand blocks to the sink.");
  }

  FakeAacEncoder failing_encoder;
  failing_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  olouie::audio::AacEncodeSink failing_sink(&failing_encoder, &store);
  olouie::audio::PreparedPcmQueue failing_queue(3);
  if (!failing_queue.TryPush(MakePreparedBlock(10)).Succeeded() ||
      !failing_queue.TryPush(MakePreparedBlock(20)).Succeeded()) {
    return Fail("Audio encode worker failure queue setup failed.");
  }

  olouie::audio::AudioEncodeWorker failing_worker(&failing_queue,
                                                  &failing_sink);
  result = failing_worker.DrainAllQueuedBlocks();
  if (result.status != olouie::audio::AudioEncodeWorkerStatus::SinkError ||
      result.submitted_block_count != 0 ||
      failing_encoder.submit_count != 1 || failing_queue.size() != 1 ||
      failing_worker.stats().sink_error_count != 1) {
    return Fail("Audio encode worker should stop on sink failure.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyCapturedPcmQueueWriterBoundary() {
  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::PcmPacketTiming timing;
  std::wstring error;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for queue writer failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  olouie::audio::PreparedPcmQueue queue(2);
  olouie::audio::CapturedPcmQueueWriter invalid_writer(nullptr, float_format,
                                                       48000);
  if (invalid_writer.QueueCapturedPcm(float_format, packet, 0,
                                      std::span<const std::byte>())
          .status != olouie::audio::CapturedPcmQueueStatus::InvalidConfig) {
    return Fail("Captured PCM queue writer should reject missing queue.");
  }

  olouie::audio::CapturedPcmQueueWriter writer(&queue, float_format, 48000);
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  auto result = writer.QueueCapturedPcm(
      float_format, packet, 1234567, BytesFromFloats(float_samples));
  if (!result.Succeeded() || result.prepared_frame_count != 4 ||
      result.queued_byte_count != 16 || queue.size() != 1) {
    return Fail("Captured PCM queue writer did not queue converted PCM.");
  }

  olouie::audio::PreparedPcmBlock queued;
  if (!queue.TryPop(&queued) || queued.input.pts_ns != 1234567 ||
      queued.input.duration_ns != timing.duration_ns ||
      queued.input.frame_count != 4 || queued.data.size() != 16) {
    return Fail("Captured PCM queued block metadata is incorrect.");
  }

  const std::vector<int16_t> expected_samples{
      -32768, -16384, 0, 16384, 32767, 32767, -32768, 8192};
  for (size_t index = 0; index < expected_samples.size(); ++index) {
    if (ReadSigned16Sample(queued.data, index) != expected_samples[index]) {
      return Fail("Captured PCM queue writer conversion is incorrect.");
    }
  }

  const auto s16_format = olouie::audio::MakeSigned16PcmFormat(48000, 2);
  olouie::audio::PreparedPcmQueue silent_queue(2);
  olouie::audio::CapturedPcmQueueWriter silent_writer(&silent_queue,
                                                      s16_format, 48000);
  olouie::audio::PcmPacketInfo silent_packet{timing, true};
  result = silent_writer.QueueCapturedPcm(
      s16_format, silent_packet, 2000000, std::span<const std::byte>());
  if (!result.Succeeded() || !silent_queue.TryPop(&queued) ||
      queued.data.size() != 16) {
    return Fail("Captured PCM silent packet was not queued.");
  }

  for (size_t index = 0; index < queued.data.size(); ++index) {
    if (queued.data[index] != std::byte{0}) {
      return Fail("Captured PCM silent packet should queue zero samples.");
    }
  }

  auto stats = silent_writer.stats();
  if (stats.attempted_packet_count != 1 || stats.silent_packet_count != 1 ||
      stats.prepared_block_count != 1 || stats.queued_block_count != 1 ||
      stats.queued_frame_count != 4) {
    return Fail("Captured PCM silent queue stats are incorrect.");
  }

  olouie::audio::CapturedPcmQueueWriter resample_writer(&queue, float_format,
                                                        44100);
  result = resample_writer.QueueCapturedPcm(
      float_format, packet, 0, BytesFromFloats(float_samples));
  if (!result.Succeeded() || result.prepared_frame_count != 3 ||
      result.queued_byte_count != 12 ||
      resample_writer.stats().resampled_packet_count != 1 ||
      resample_writer.stats().resampled_frame_count != 3) {
    return Fail("Captured PCM queue writer should resample capture packets.");
  }

  result = writer.QueueCapturedPcm(
      float_format, packet, -1, BytesFromFloats(float_samples));
  if (result.status != olouie::audio::CapturedPcmQueueStatus::InvalidPacket) {
    return Fail("Captured PCM queue writer should reject negative PTS.");
  }

  const std::vector<int16_t> changed_format_samples(8, 123);
  result = writer.QueueCapturedPcm(
      s16_format, packet, 0, BytesFromSigned16(changed_format_samples));
  if (!result.Succeeded() ||
      writer.stats().input_format_change_count != 1) {
    return Fail("Captured PCM queue writer should accept compatible endpoint "
                "format changes.");
  }

  olouie::audio::PreparedPcmQueue changed_silence_queue(2);
  olouie::audio::CapturedPcmQueueWriter changed_silence_writer(
      &changed_silence_queue, float_format, 48000);
  const auto mono_s16_format =
      olouie::audio::MakeSigned16PcmFormat(48000, 1);
  result = changed_silence_writer.QueueCapturedPcm(
      mono_s16_format, silent_packet, 4000000,
      std::span<const std::byte>());
  if (!result.Succeeded() || !changed_silence_queue.TryPop(&queued) ||
      queued.input.frame_count != 4 || queued.data.size() != 16 ||
      changed_silence_writer.stats().input_format_change_count != 1) {
    return Fail("Captured PCM queue writer should adapt silent packets after "
                "an endpoint format change.");
  }

  olouie::audio::PreparedPcmQueue full_queue(1);
  olouie::audio::CapturedPcmQueueWriter full_writer(&full_queue, s16_format,
                                                    48000);
  const std::vector<int16_t> s16_samples(8, 7);
  if (!full_writer
           .QueueCapturedPcm(s16_format, packet, 0,
                             BytesFromSigned16(s16_samples))
           .Succeeded()) {
    return Fail("Captured PCM queue writer setup push failed.");
  }

  result = full_writer.QueueCapturedPcm(s16_format, packet, 1000,
                                        BytesFromSigned16(s16_samples));
  if (result.status != olouie::audio::CapturedPcmQueueStatus::QueueRejected ||
      full_writer.stats().queue_rejection_count != 1 ||
      full_queue.size() != 1) {
    return Fail("Captured PCM queue writer should surface queue rejection.");
  }

  return 0;
}

int VerifyAudioTrackEncodeChainBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for chain test failed: " << error
               << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioChainTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for chain failed: " << error << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioTrackEncodeChainConfig config;
  config.input_format = float_format;
  config.output_sample_rate = 48000;
  config.queue_capacity = 4;

  FakeAacEncoder fake_encoder;
  olouie::audio::EncodedAacPacket drain_packet;
  drain_packet.track_id = plan.tracks[0].track_id;
  drain_packet.pts_ns = 0;
  drain_packet.dts_ns = 0;
  drain_packet.duration_ns = 1000000;
  drain_packet.data = {std::byte{0x61}, std::byte{0x62}};
  fake_encoder.drain_packets = {drain_packet};

  olouie::audio::EncodedAacPacket flush_packet;
  flush_packet.track_id = plan.tracks[0].track_id;
  flush_packet.pts_ns = 5000000;
  flush_packet.dts_ns = 5000000;
  flush_packet.duration_ns = 1000000;
  flush_packet.data = {std::byte{0x63}, std::byte{0x64}};
  fake_encoder.flush_packets = {flush_packet};

  olouie::audio::AudioTrackEncodeChain chain(config, &fake_encoder, &store);
  if (!chain.IsConfigured() || chain.config().queue_capacity != 4 ||
      !chain.queue_empty()) {
    return Fail("Audio track encode chain configuration is incorrect.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for chain test failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  auto queue_result = chain.QueueCapturedPcm(
      float_format, packet, 1234567, BytesFromFloats(float_samples));
  if (!queue_result.Succeeded() || chain.queued_block_count() != 1) {
    return Fail("Audio track encode chain did not queue captured PCM.");
  }

  auto drain_result = chain.DrainQueuedBlocks(1);
  if (!drain_result.Succeeded() ||
      drain_result.submitted_block_count != 1 ||
      fake_encoder.submit_count != 1 || !chain.queue_empty() ||
      fake_encoder.last_input.pts_ns != 1234567 ||
      fake_encoder.last_input.frame_count != 4 ||
      fake_encoder.last_pcm_size != 16) {
    return Fail("Audio track encode chain did not drain queued PCM.");
  }

  olouie::audio::PcmPacketInfo silent_packet{timing, true};
  if (!chain
           .QueueCapturedPcm(float_format, silent_packet, 2000000,
                             std::span<const std::byte>())
           .Succeeded() ||
      !chain.QueueCapturedPcm(float_format, packet, 3000000,
                              BytesFromFloats(float_samples))
           .Succeeded()) {
    return Fail("Audio track encode chain multi-queue setup failed.");
  }

  drain_result = chain.DrainAllQueuedBlocks();
  if (!drain_result.Succeeded() ||
      drain_result.submitted_block_count != 2 ||
      fake_encoder.submit_count != 3 || !chain.queue_empty()) {
    return Fail("Audio track encode chain did not drain all queued PCM.");
  }

  const auto writer_stats = chain.queue_writer_stats();
  if (writer_stats.attempted_packet_count != 3 ||
      writer_stats.silent_packet_count != 1 ||
      writer_stats.queued_block_count != 3) {
    return Fail("Audio track encode chain queue writer stats are incorrect.");
  }

  const auto queue_stats = chain.queue_stats();
  if (queue_stats.pushed_block_count != 3 ||
      queue_stats.popped_block_count != 3) {
    return Fail("Audio track encode chain queue stats are incorrect.");
  }

  const auto worker_stats = chain.worker_stats();
  if (worker_stats.submitted_block_count != 3 ||
      worker_stats.submitted_frame_count != 12) {
    return Fail("Audio track encode chain worker stats are incorrect.");
  }

  auto flush_result = chain.Flush();
  if (!flush_result.Succeeded() || fake_encoder.flush_count != 1) {
    return Fail("Audio track encode chain flush failed.");
  }

  const auto sink_stats = chain.sink_stats();
  if (sink_stats.submitted_block_count != 3 ||
      sink_stats.appended_packet_count != 4) {
    return Fail("Audio track encode chain sink stats are incorrect.");
  }

  const auto range = store.QueryRange(0, 10000000, false);
  if (range.packets.size() != 4) {
    return Fail("Audio track encode chain PacketStore output is incorrect.");
  }

  olouie::audio::AudioTrackEncodeChainConfig invalid_config;
  invalid_config.input_format = float_format;
  invalid_config.output_sample_rate = 48000;
  invalid_config.queue_capacity = 0;
  olouie::audio::AudioTrackEncodeChain invalid_chain(
      invalid_config, &fake_encoder, &store);
  if (invalid_chain.IsConfigured()) {
    return Fail("Audio track encode chain should expose invalid config.");
  }

  olouie::audio::AudioTrackEncodeChainConfig small_config = config;
  small_config.queue_capacity = 1;
  olouie::audio::AudioTrackEncodeChain full_chain(small_config, &fake_encoder,
                                                  &store);
  if (!full_chain
           .QueueCapturedPcm(float_format, packet, 4000000,
                             BytesFromFloats(float_samples))
           .Succeeded() ||
      full_chain
              .QueueCapturedPcm(float_format, packet, 5000000,
                                BytesFromFloats(float_samples))
              .status !=
          olouie::audio::CapturedPcmQueueStatus::QueueRejected) {
    return Fail("Audio track encode chain should surface queue rejection.");
  }

  FakeAacEncoder failing_encoder;
  failing_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  olouie::audio::AudioTrackEncodeChain failing_chain(config, &failing_encoder,
                                                     &store);
  if (!failing_chain
           .QueueCapturedPcm(float_format, packet, 6000000,
                             BytesFromFloats(float_samples))
           .Succeeded()) {
    return Fail("Audio track encode chain failure setup queue failed.");
  }

  drain_result = failing_chain.DrainAllQueuedBlocks();
  if (drain_result.status != olouie::audio::AudioEncodeWorkerStatus::SinkError ||
      failing_encoder.submit_count != 1 ||
      failing_chain.worker_stats().sink_error_count != 1) {
    return Fail("Audio track encode chain should surface sink failure.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioEncodeSessionBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for session test failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 2) {
    return Fail("Audio encode session test expected two planned tracks.");
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioSessionTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for session failed: " << error
               << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  const auto mic_s16_format = olouie::audio::MakeSigned16PcmFormat(48000, 1);
  olouie::audio::AudioTrackEncodeChainConfig chain_config;
  chain_config.input_format = float_format;
  chain_config.output_sample_rate = 48000;
  chain_config.queue_capacity = 2;
  olouie::audio::AudioTrackEncodeChainConfig mic_chain_config = chain_config;
  mic_chain_config.input_format = mic_s16_format;

  FakeAacEncoder system_encoder;
  olouie::audio::EncodedAacPacket system_packet;
  system_packet.track_id = plan.tracks[0].track_id;
  system_packet.pts_ns = 0;
  system_packet.dts_ns = 0;
  system_packet.duration_ns = 1000000;
  system_packet.data = {std::byte{0x71}, std::byte{0x72}};
  system_encoder.drain_packets = {system_packet};

  olouie::audio::EncodedAacPacket system_flush_packet;
  system_flush_packet.track_id = plan.tracks[0].track_id;
  system_flush_packet.pts_ns = 5000000;
  system_flush_packet.dts_ns = 5000000;
  system_flush_packet.duration_ns = 1000000;
  system_flush_packet.data = {std::byte{0x73}};
  system_encoder.flush_packets = {system_flush_packet};

  FakeAacEncoder mic_encoder;
  olouie::audio::EncodedAacPacket mic_packet;
  mic_packet.track_id = plan.tracks[1].track_id;
  mic_packet.pts_ns = 1000000;
  mic_packet.dts_ns = 1000000;
  mic_packet.duration_ns = 1000000;
  mic_packet.data = {std::byte{0x81}, std::byte{0x82}};
  mic_encoder.drain_packets = {mic_packet};

  olouie::audio::EncodedAacPacket mic_flush_packet;
  mic_flush_packet.track_id = plan.tracks[1].track_id;
  mic_flush_packet.pts_ns = 6000000;
  mic_flush_packet.dts_ns = 6000000;
  mic_flush_packet.duration_ns = 1000000;
  mic_flush_packet.data = {std::byte{0x83}};
  mic_encoder.flush_packets = {mic_flush_packet};

  const std::array bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &system_encoder},
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[1].track_id, mic_chain_config, &mic_encoder},
  };

  olouie::audio::AudioEncodeSession session(bindings, &store);
  if (!session.IsConfigured() || session.track_count() != 2 ||
      !session.HasTrack(plan.tracks[0].track_id) ||
      !session.HasTrack(plan.tracks[1].track_id)) {
    return Fail("Audio encode session configuration is incorrect.");
  }

  olouie::audio::AudioEncodeSession null_store_session(bindings, nullptr);
  if (null_store_session.IsConfigured()) {
    return Fail("Audio encode session should reject a null PacketStore.");
  }

  const std::array duplicate_bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &system_encoder},
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &mic_encoder},
  };
  olouie::audio::AudioEncodeSession duplicate_session(duplicate_bindings,
                                                      &store);
  if (duplicate_session.IsConfigured()) {
    return Fail("Audio encode session should reject duplicate track ids.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for session test failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  const std::vector<int16_t> mic_samples{-100, 200, -300, 400};

  if (session.QueueCapturedPcm(9999, float_format, packet, 0,
                               BytesFromFloats(float_samples))
          .status != olouie::audio::AudioEncodeSessionStatus::UnknownTrack) {
    return Fail("Audio encode session should reject unknown track ids.");
  }

  if (!session
           .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                             1000000,
                             BytesFromFloats(float_samples))
           .Succeeded() ||
      !session
           .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                             2000000,
                             BytesFromFloats(float_samples))
           .Succeeded() ||
      !session
           .QueueCapturedPcm(plan.tracks[1].track_id, mic_s16_format, packet,
                             3000000,
                             BytesFromSigned16(mic_samples))
           .Succeeded()) {
    return Fail("Audio encode session failed to queue routed packets.");
  }

  const auto surround_float_format = olouie::audio::MakePcmStreamFormat(
      48000, 6, 32, 24, 1152000,
      olouie::audio::PcmSampleEncoding::Float);
  const std::vector<float> surround_samples(24, 0.25f);
  if (session
          .QueueCapturedPcm(plan.tracks[1].track_id,
                            surround_float_format, packet, 3500000,
                            BytesFromFloats(surround_samples))
          .status != olouie::audio::AudioEncodeSessionStatus::QueueError ||
      session.stats().queue_failure_count != 1) {
    return Fail("Audio encode session should reject mismatched track formats.");
  }

  if (session.QueuedBlockCount(plan.tracks[0].track_id) != 2 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 1) {
    return Fail("Audio encode session routed packets to the wrong queues.");
  }

  auto drain = session.DrainTrack(plan.tracks[0].track_id, 1);
  if (!drain.Succeeded() || drain.processed_block_count != 1 ||
      system_encoder.submit_count != 1 ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 1 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 1) {
    return Fail("Audio encode session did not drain the requested track.");
  }

  drain = session.DrainAllQueuedBlocks();
  if (!drain.Succeeded() || drain.processed_block_count != 2 ||
      system_encoder.submit_count != 2 || mic_encoder.submit_count != 1 ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 0 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 0 ||
      mic_encoder.last_pcm_size != 8) {
    return Fail("Audio encode session did not drain all tracks.");
  }

  auto flush = session.FlushAllTracks();
  if (!flush.Succeeded() || flush.processed_block_count != 2 ||
      system_encoder.flush_count != 1 || mic_encoder.flush_count != 1) {
    return Fail("Audio encode session did not flush all tracks.");
  }

  auto stats = session.stats();
  if (stats.queued_packet_count != 3 || stats.drained_block_count != 3 ||
      stats.flushed_track_count != 2 || stats.queue_failure_count != 1 ||
      stats.drain_failure_count != 0 || stats.flush_failure_count != 0) {
    return Fail("Audio encode session stats are incorrect.");
  }

  const auto range = store.QueryRange(0, 10000000, false);
  if (range.packets.size() != 5) {
    return Fail("Audio encode session PacketStore output is incorrect.");
  }

  olouie::audio::AudioTrackEncodeChainConfig small_config = chain_config;
  small_config.queue_capacity = 1;
  const std::array small_bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, small_config, &system_encoder},
  };
  olouie::audio::AudioEncodeSession full_session(small_bindings, &store);
  if (!full_session
           .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                             4000000,
                             BytesFromFloats(float_samples))
           .Succeeded() ||
      full_session
              .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                                5000000,
                                BytesFromFloats(float_samples))
              .status !=
          olouie::audio::AudioEncodeSessionStatus::QueueError ||
      full_session.stats().queue_failure_count != 1) {
    return Fail("Audio encode session should surface queue errors.");
  }

  FakeAacEncoder failing_encoder;
  failing_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  const std::array failing_bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &failing_encoder},
  };
  olouie::audio::AudioEncodeSession failing_session(failing_bindings, &store);
  if (!failing_session
           .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                             6000000,
                             BytesFromFloats(float_samples))
           .Succeeded()) {
    return Fail("Audio encode session failure setup queue failed.");
  }

  drain = failing_session.DrainAllQueuedBlocks();
  if (drain.status != olouie::audio::AudioEncodeSessionStatus::DrainError ||
      failing_encoder.submit_count != 1 ||
      failing_session.stats().drain_failure_count != 1) {
    return Fail("Audio encode session should surface drain errors.");
  }

  FakeAacEncoder flush_failing_encoder;
  flush_failing_encoder.flush_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  const std::array flush_failing_bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &flush_failing_encoder},
  };
  olouie::audio::AudioEncodeSession flush_failing_session(
      flush_failing_bindings, &store);
  flush = flush_failing_session.FlushAllTracks();
  if (flush.status != olouie::audio::AudioEncodeSessionStatus::FlushError ||
      flush_failing_session.stats().flush_failure_count != 1) {
    return Fail("Audio encode session should surface flush errors.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioEncodeSessionBoundedDrainBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for bounded drain failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 2) {
    return Fail("Audio bounded drain test expected two tracks.");
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioBoundedDrainTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for bounded drain failed: " << error
               << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioTrackEncodeChainConfig chain_config;
  chain_config.input_format = float_format;
  chain_config.output_sample_rate = 48000;
  chain_config.queue_capacity = 4;

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  const std::array bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &system_encoder},
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[1].track_id, chain_config, &mic_encoder},
  };

  olouie::audio::AudioEncodeSession session(bindings, &store);
  if (!session.IsConfigured()) {
    return Fail("Audio bounded drain session should be configured.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for bounded drain failed: " << error << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};

  for (int index = 0; index < 3; ++index) {
    if (!session
             .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                               1000000 + (index * 1000000),
                               BytesFromFloats(float_samples))
             .Succeeded()) {
      return Fail("Audio bounded drain failed to queue system packets.");
    }
  }

  for (int index = 0; index < 2; ++index) {
    if (!session
             .QueueCapturedPcm(plan.tracks[1].track_id, float_format, packet,
                               5000000 + (index * 1000000),
                               BytesFromFloats(float_samples))
             .Succeeded()) {
      return Fail("Audio bounded drain failed to queue mic packets.");
    }
  }

  auto drain = session.DrainQueuedBlocks(0);
  if (!drain.Succeeded() || drain.processed_block_count != 0 ||
      system_encoder.submit_count != 0 || mic_encoder.submit_count != 0 ||
      session.stats().drained_block_count != 0) {
    return Fail("Audio bounded drain zero budget should be a no-op.");
  }

  drain = session.DrainQueuedBlocks(3);
  if (!drain.Succeeded() || drain.processed_block_count != 3 ||
      system_encoder.submit_count != 2 || mic_encoder.submit_count != 1 ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 1 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 1) {
    return Fail("Audio bounded drain should fairly drain within budget.");
  }

  drain = session.DrainQueuedBlocks(10);
  auto stats = session.stats();
  if (!drain.Succeeded() || drain.processed_block_count != 2 ||
      system_encoder.submit_count != 3 || mic_encoder.submit_count != 2 ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 0 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 0 ||
      stats.queued_packet_count != 5 || stats.drained_block_count != 5 ||
      stats.drain_failure_count != 0) {
    return Fail("Audio bounded drain should finish remaining packets.");
  }

  olouie::audio::AudioEncodeSession empty_session({}, &store);
  if (empty_session.DrainQueuedBlocks(1).status !=
      olouie::audio::AudioEncodeSessionStatus::InvalidConfig) {
    return Fail("Audio bounded drain should reject unconfigured sessions.");
  }

  FakeAacEncoder ok_encoder;
  FakeAacEncoder failing_encoder;
  failing_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  const std::array failing_bindings{
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[0].track_id, chain_config, &ok_encoder},
      olouie::audio::AudioEncodeSessionTrack{
          plan.tracks[1].track_id, chain_config, &failing_encoder},
  };

  olouie::audio::AudioEncodeSession failing_session(failing_bindings, &store);
  if (!failing_session
           .QueueCapturedPcm(plan.tracks[0].track_id, float_format, packet,
                             8000000,
                             BytesFromFloats(float_samples))
           .Succeeded() ||
      !failing_session
           .QueueCapturedPcm(plan.tracks[1].track_id, float_format, packet,
                             9000000,
                             BytesFromFloats(float_samples))
           .Succeeded()) {
    return Fail("Audio bounded drain failure setup queue failed.");
  }

  drain = failing_session.DrainQueuedBlocks(2);
  stats = failing_session.stats();
  if (drain.status != olouie::audio::AudioEncodeSessionStatus::DrainError ||
      drain.track_id != plan.tracks[1].track_id ||
      drain.processed_block_count != 1 || ok_encoder.submit_count != 1 ||
      failing_encoder.submit_count != 1 || stats.drained_block_count != 1 ||
      stats.drain_failure_count != 1) {
    return Fail("Audio bounded drain should report the failing track.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioEncodeSessionBindingBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for binding test failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 2) {
    return Fail("Audio encode session binding test expected two tracks.");
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  const auto mic_s16_format = olouie::audio::MakeSigned16PcmFormat(48000, 1);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 3;
  binding_options.overflow_policy =
      olouie::audio::PreparedPcmOverflowPolicy::DropOldest;

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  const std::array slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
  };
  const std::array format_slots{
      FormatSlot(plan.tracks[1].track_id, mic_s16_format),
      FormatSlot(plan.tracks[0].track_id, float_format),
  };

  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, &tracks);
  if (!binding.Succeeded() || tracks.size() != plan.tracks.size()) {
    std::wcerr << L"Audio encode session binding failed: " << binding.message
               << L'\n';
    return 1;
  }

  if (tracks[0].track_id != plan.tracks[0].track_id ||
      tracks[0].encoder != &system_encoder ||
      tracks[1].track_id != plan.tracks[1].track_id ||
      tracks[1].encoder != &mic_encoder) {
    return Fail("Audio encode session binding did not preserve plan order.");
  }

  if (!olouie::audio::SamePcmStreamFormat(tracks[0].config.input_format,
                                          float_format) ||
      !olouie::audio::SamePcmStreamFormat(tracks[1].config.input_format,
                                          mic_s16_format) ||
      tracks[0].config.output_sample_rate != float_format.sample_rate ||
      tracks[1].config.output_sample_rate != mic_s16_format.sample_rate ||
      tracks[0].config.queue_capacity != 3 ||
      tracks[1].config.queue_capacity != 3 ||
      tracks[0].config.overflow_policy !=
          olouie::audio::PreparedPcmOverflowPolicy::DropOldest ||
      tracks[1].config.overflow_policy !=
          olouie::audio::PreparedPcmOverflowPolicy::DropOldest) {
    return Fail("Audio encode session binding copied config incorrectly.");
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioBindingTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for binding failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession session(tracks, &store);
  if (!session.IsConfigured() || session.track_count() != 2 ||
      !session.HasTrack(plan.tracks[0].track_id) ||
      !session.HasTrack(plan.tracks[1].track_id)) {
    return Fail("Audio encode session binding produced unusable tracks.");
  }

  store.Close();
  std::filesystem::remove_all(root);

  const std::array missing_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, missing_slots, &tracks);
  if (binding.status !=
          olouie::audio::AudioEncodeSessionBindingStatus::MissingEncoder ||
      !tracks.empty()) {
    return Fail("Audio encode session binding should reject missing encoders.");
  }

  const std::array duplicate_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &mic_encoder},
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, duplicate_slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::DuplicateEncoder) {
    return Fail("Audio encode session binding should reject duplicate slots.");
  }

  const std::array unexpected_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{9999, &mic_encoder},
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, unexpected_slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::UnexpectedEncoder) {
    return Fail("Audio encode session binding should reject unknown tracks.");
  }

  const std::array null_encoder_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, nullptr},
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, null_encoder_slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::MissingEncoder) {
    return Fail("Audio encode session binding should reject null encoders.");
  }

  olouie::audio::AudioEncodeSessionBindingOptions invalid_options =
      binding_options;
  invalid_options.queue_capacity = 0;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, invalid_options, format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::InvalidOptions) {
    return Fail("Audio encode session binding should reject invalid options.");
  }

  olouie::audio::AudioTrackPlan empty_plan;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      empty_plan, binding_options, format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::InvalidPlan) {
    return Fail("Audio encode session binding should reject empty plans.");
  }

  olouie::audio::AudioTrackPlan invalid_packet_plan = plan;
  invalid_packet_plan.packet_tracks.pop_back();
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      invalid_packet_plan, binding_options, format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::InvalidPlan) {
    return Fail("Audio encode session binding should reject malformed plans.");
  }

  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, nullptr);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::InvalidOptions) {
    return Fail("Audio encode session binding should reject null outputs.");
  }

  const std::array missing_format_slots{
      FormatSlot(plan.tracks[0].track_id, float_format),
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, missing_format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::MissingFormat) {
    return Fail("Audio encode session binding should reject missing formats.");
  }

  const std::array duplicate_format_slots{
      FormatSlot(plan.tracks[0].track_id, float_format),
      FormatSlot(plan.tracks[0].track_id, mic_s16_format),
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, duplicate_format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::DuplicateFormat) {
    return Fail("Audio encode session binding should reject duplicate formats.");
  }

  const std::array unexpected_format_slots{
      FormatSlot(plan.tracks[0].track_id, float_format),
      FormatSlot(plan.tracks[1].track_id, mic_s16_format),
      FormatSlot(9999, float_format),
  };
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, unexpected_format_slots, slots, &tracks);
  if (binding.status !=
      olouie::audio::AudioEncodeSessionBindingStatus::UnexpectedFormat) {
    return Fail("Audio encode session binding should reject unknown formats.");
  }

  return 0;
}

int VerifyAudioSourceSessionDispatcherBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for source dispatcher failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 2) {
    return Fail("Audio source dispatcher test expected two direct tracks.");
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 3;
  const auto format_slots = FormatSlotsForPlan(plan, float_format);

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  const std::array slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
  };

  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, &tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Audio source dispatcher binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieAudioDispatcherTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for dispatcher failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::AudioSourceRouter router(plan);
  olouie::audio::AudioEncodeSession session(tracks, &store);
  olouie::audio::AudioSourceSessionDispatcher dispatcher(&router, &session);
  if (!dispatcher.IsConfigured()) {
    return Fail("Audio source session dispatcher should be configured.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for source dispatcher failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};

  auto dispatch = dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0}, float_format,
      packet, 1000000, BytesFromFloats(float_samples));
  if (!dispatch.Succeeded() ||
      dispatch.track_id != plan.tracks[0].track_id ||
      dispatch.route_status != olouie::audio::AudioSourceRouteStatus::Success ||
      dispatch.queue_status !=
          olouie::audio::AudioEncodeSessionStatus::Success) {
    return Fail("Audio source session dispatcher did not queue system PCM.");
  }

  dispatch = dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::Microphone, 0}, float_format, packet,
      2000000, BytesFromFloats(float_samples));
  if (!dispatch.Succeeded() ||
      dispatch.track_id != plan.tracks[1].track_id ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 1 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 1 ||
      session.stats().queued_packet_count != 2) {
    return Fail("Audio source session dispatcher did not route mic PCM.");
  }

  dispatch = dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::ProcessLoopback, 0}, float_format,
      packet, 3000000, BytesFromFloats(float_samples));
  if (dispatch.status !=
          olouie::audio::AudioSourceSessionDispatchStatus::RouteError ||
      dispatch.route_status !=
          olouie::audio::AudioSourceRouteStatus::SourceNotEnabled ||
      dispatch.queue_status !=
          olouie::audio::AudioEncodeSessionStatus::InvalidConfig ||
      dispatch.track_id != 0 || session.stats().queued_packet_count != 2) {
    return Fail("Audio source session dispatcher should surface route errors.");
  }

  dispatch = dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::DefaultMixed, 0}, float_format, packet,
      4000000, BytesFromFloats(float_samples));
  if (dispatch.status !=
          olouie::audio::AudioSourceSessionDispatchStatus::RouteError ||
      dispatch.route_status !=
          olouie::audio::AudioSourceRouteStatus::InvalidSource) {
    return Fail("Audio source session dispatcher should reject mixer sources.");
  }

  olouie::audio::AudioSourceSessionDispatcher null_dispatcher(nullptr,
                                                              &session);
  if (null_dispatcher.IsConfigured() ||
      null_dispatcher
              .QueueCapturedPcm(
                  {olouie::audio::AudioTrackKind::SystemLoopback, 0},
                  float_format, packet, 5000000,
                  BytesFromFloats(float_samples))
              .status !=
          olouie::audio::AudioSourceSessionDispatchStatus::InvalidConfig) {
    return Fail("Audio source session dispatcher should reject null routing.");
  }

  olouie::audio::AudioTrackPlan empty_plan;
  olouie::audio::AudioSourceRouter empty_router(empty_plan);
  olouie::audio::AudioSourceSessionDispatcher empty_router_dispatcher(
      &empty_router, &session);
  dispatch = empty_router_dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0}, float_format,
      packet, 6000000, BytesFromFloats(float_samples));
  if (dispatch.status !=
          olouie::audio::AudioSourceSessionDispatchStatus::RouteError ||
      dispatch.route_status != olouie::audio::AudioSourceRouteStatus::InvalidPlan) {
    return Fail("Audio source session dispatcher should surface bad routers.");
  }

  olouie::audio::AudioEncodeSession empty_session({}, &store);
  olouie::audio::AudioSourceSessionDispatcher empty_session_dispatcher(
      &router, &empty_session);
  dispatch = empty_session_dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0}, float_format,
      packet, 7000000, BytesFromFloats(float_samples));
  if (dispatch.status !=
          olouie::audio::AudioSourceSessionDispatchStatus::QueueError ||
      dispatch.route_status != olouie::audio::AudioSourceRouteStatus::Success ||
      dispatch.queue_status !=
          olouie::audio::AudioEncodeSessionStatus::InvalidConfig ||
      dispatch.track_id != plan.tracks[0].track_id) {
    return Fail("Audio source session dispatcher should surface bad sessions.");
  }

  olouie::audio::AudioEncodeSessionBindingOptions small_options =
      binding_options;
  small_options.queue_capacity = 1;
  FakeAacEncoder full_encoder;
  FakeAacEncoder unused_encoder;
  const std::array full_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &full_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &unused_encoder},
  };
  std::vector<olouie::audio::AudioEncodeSessionTrack> full_tracks;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, small_options, format_slots, full_slots, &full_tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Audio source dispatcher queue-full binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession full_session(full_tracks, &store);
  olouie::audio::AudioSourceSessionDispatcher full_dispatcher(&router,
                                                              &full_session);
  if (!full_dispatcher
           .QueueCapturedPcm(
               {olouie::audio::AudioTrackKind::SystemLoopback, 0},
               float_format, packet, 8000000,
               BytesFromFloats(float_samples))
           .Succeeded()) {
    return Fail("Audio source session dispatcher queue-full setup failed.");
  }

  dispatch = full_dispatcher.QueueCapturedPcm(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0}, float_format,
      packet, 9000000, BytesFromFloats(float_samples));
  if (dispatch.status !=
          olouie::audio::AudioSourceSessionDispatchStatus::QueueError ||
      dispatch.route_status != olouie::audio::AudioSourceRouteStatus::Success ||
      dispatch.queue_status !=
          olouie::audio::AudioEncodeSessionStatus::QueueError ||
      dispatch.track_id != plan.tracks[0].track_id ||
      full_session.stats().queue_failure_count != 1) {
    return Fail("Audio source session dispatcher should surface queue errors.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyCapturedPcmSessionSinkBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for captured session sink failed: "
               << error << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 3;
  const auto format_slots = FormatSlotsForPlan(plan, float_format);

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  const std::array slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
  };

  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, &tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Captured session sink binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieCapturedPcmSessionSinkTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for captured session sink failed: "
               << error << L'\n';
    return 1;
  }

  olouie::audio::AudioSourceRouter router(plan);
  olouie::audio::AudioEncodeSession session(tracks, &store);
  olouie::audio::AudioSourceSessionDispatcher dispatcher(&router, &session);
  olouie::audio::CapturedPcmSessionSink sink(&dispatcher, 600);
  if (!sink.IsConfigured()) {
    return Fail("Captured PCM session sink should be configured.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for captured session sink failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  const auto pcm_data = BytesFromFloats(float_samples);
  olouie::audio::CapturedPcmPacket captured{
      olouie::audio::CapturedAudioSource{
          olouie::audio::AudioTrackKind::SystemLoopback, 0},
      float_format, packet,
      std::span<const std::byte>(pcm_data.data(), pcm_data.size())};

  auto delivered = olouie::audio::DispatchCapturedPcm(&sink, captured);
  auto stats = sink.stats();
  if (!delivered.Succeeded() || stats.received_packet_count != 1 ||
      stats.queued_packet_count != 1 ||
      sink.last_dispatch_result().track_id != plan.tracks[0].track_id ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 1) {
    return Fail("Captured PCM session sink did not queue a valid packet.");
  }

  auto drain = session.DrainTrack(plan.tracks[0].track_id, 1);
  if (!drain.Succeeded() || system_encoder.submit_count != 1 ||
      system_encoder.last_input.pts_ns != timing.qpc_position_ns - 600 ||
      system_encoder.last_pcm_size != 16) {
    return Fail("Captured PCM session sink did not preserve queued timing.");
  }

  olouie::audio::CapturedPcmPacket process_source = captured;
  process_source.source = {olouie::audio::AudioTrackKind::ProcessLoopback, 0};
  delivered = olouie::audio::DispatchCapturedPcm(&sink, process_source);
  stats = sink.stats();
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::RouteError ||
      stats.route_error_count != 1 ||
      sink.last_dispatch_result().status !=
          olouie::audio::AudioSourceSessionDispatchStatus::RouteError ||
      sink.last_dispatch_result().route_status !=
          olouie::audio::AudioSourceRouteStatus::SourceNotEnabled) {
    return Fail("Captured PCM session sink should surface route errors.");
  }

  olouie::audio::CapturedPcmPacket invalid_packet = captured;
  invalid_packet.pcm_bytes = std::span<const std::byte>();
  delivered = sink.OnCapturedPcm(invalid_packet);
  stats = sink.stats();
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::InvalidPacket ||
      stats.invalid_packet_count != 1) {
    return Fail("Captured PCM session sink should reject invalid packets.");
  }

  olouie::audio::CapturedPcmSessionSink null_sink(nullptr);
  delivered = olouie::audio::DispatchCapturedPcm(&null_sink, captured);
  if (null_sink.IsConfigured() ||
      delivered.status !=
          olouie::audio::CapturedPcmSinkStatus::InvalidConfig ||
      null_sink.stats().invalid_config_count != 1) {
    return Fail("Captured PCM session sink should reject null dispatchers.");
  }

  olouie::audio::AudioEncodeSessionBindingOptions small_options =
      binding_options;
  small_options.queue_capacity = 1;
  FakeAacEncoder full_encoder;
  FakeAacEncoder unused_encoder;
  const std::array full_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &full_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &unused_encoder},
  };
  std::vector<olouie::audio::AudioEncodeSessionTrack> full_tracks;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, small_options, format_slots, full_slots, &full_tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Captured session sink queue-full binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession full_session(full_tracks, &store);
  olouie::audio::AudioSourceSessionDispatcher full_dispatcher(&router,
                                                              &full_session);
  olouie::audio::CapturedPcmSessionSink full_sink(&full_dispatcher);
  if (!olouie::audio::DispatchCapturedPcm(&full_sink, captured).Succeeded()) {
    return Fail("Captured PCM session sink queue-full setup failed.");
  }

  delivered = olouie::audio::DispatchCapturedPcm(&full_sink, captured);
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::QueueError ||
      full_sink.stats().queue_error_count != 1 ||
      full_sink.last_dispatch_result().queue_status !=
          olouie::audio::AudioEncodeSessionStatus::QueueError ||
      full_session.stats().queue_failure_count != 1) {
    return Fail("Captured PCM session sink should surface queue errors.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioCaptureEncodeBridgeBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for capture encode bridge failed: "
               << error << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 3;
  const auto format_slots = FormatSlotsForPlan(plan, float_format);

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  const std::array slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
  };

  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, &tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Capture encode bridge binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieAudioCaptureEncodeBridgeTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for capture encode bridge failed: "
               << error << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession session(tracks, &store);
  olouie::audio::AudioCaptureEncodeBridge bridge(plan, &session, 600);
  if (!bridge.IsConfigured() || bridge.captured_pcm_sink() == nullptr ||
      bridge.router().route_count() != 2) {
    return Fail("Audio capture encode bridge should be configured.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    std::wcerr << L"PCM timing for capture encode bridge failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::PcmPacketInfo packet{timing, false};
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};
  const auto pcm_data = BytesFromFloats(float_samples);
  olouie::audio::CapturedPcmPacket captured{
      olouie::audio::CapturedAudioSource{
          olouie::audio::AudioTrackKind::SystemLoopback, 0},
      float_format, packet,
      std::span<const std::byte>(pcm_data.data(), pcm_data.size())};

  auto delivered =
      olouie::audio::DispatchCapturedPcm(bridge.captured_pcm_sink(), captured);
  if (!delivered.Succeeded() ||
      bridge.sink_stats().received_packet_count != 1 ||
      bridge.sink_stats().queued_packet_count != 1 ||
      bridge.last_dispatch_result().track_id != plan.tracks[0].track_id ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 1) {
    return Fail("Audio capture encode bridge did not queue captured PCM.");
  }

  auto drain = bridge.DrainQueuedBlocks(1);
  if (!drain.Succeeded() || drain.processed_block_count != 1 ||
      system_encoder.submit_count != 1 ||
      system_encoder.last_input.pts_ns != timing.qpc_position_ns - 600 ||
      system_encoder.last_pcm_size != 16) {
    return Fail("Audio capture encode bridge did not drain queued PCM.");
  }

  olouie::audio::CapturedPcmPacket mic_source = captured;
  mic_source.source = {olouie::audio::AudioTrackKind::Microphone, 0};
  if (!olouie::audio::DispatchCapturedPcm(bridge.captured_pcm_sink(),
                                          mic_source)
           .Succeeded() ||
      !olouie::audio::DispatchCapturedPcm(bridge.captured_pcm_sink(), captured)
           .Succeeded()) {
    return Fail("Audio capture encode bridge drain setup failed.");
  }

  drain = bridge.DrainQueuedBlocks(1);
  if (!drain.Succeeded() || drain.processed_block_count != 1 ||
      system_encoder.submit_count != 2 || mic_encoder.submit_count != 0 ||
      session.QueuedBlockCount(plan.tracks[0].track_id) != 0 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 1) {
    return Fail("Audio capture encode bridge should honor drain budgets.");
  }

  drain = bridge.DrainAllQueuedBlocks();
  if (!drain.Succeeded() || drain.processed_block_count != 1 ||
      mic_encoder.submit_count != 1 ||
      session.QueuedBlockCount(plan.tracks[1].track_id) != 0) {
    return Fail("Audio capture encode bridge should drain all queued PCM.");
  }

  olouie::audio::CapturedPcmPacket process_source = captured;
  process_source.source = {olouie::audio::AudioTrackKind::ProcessLoopback, 0};
  delivered = olouie::audio::DispatchCapturedPcm(
      bridge.captured_pcm_sink(), process_source);
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::RouteError ||
      bridge.sink_stats().route_error_count != 1 ||
      bridge.last_dispatch_result().route_status !=
          olouie::audio::AudioSourceRouteStatus::SourceNotEnabled) {
    return Fail("Audio capture encode bridge should surface route errors.");
  }

  olouie::audio::AudioCaptureEncodeBridge null_session_bridge(plan, nullptr);
  delivered = olouie::audio::DispatchCapturedPcm(
      null_session_bridge.captured_pcm_sink(), captured);
  if (null_session_bridge.IsConfigured() ||
      delivered.status !=
          olouie::audio::CapturedPcmSinkStatus::InvalidConfig ||
      null_session_bridge.sink_stats().invalid_config_count != 1 ||
      null_session_bridge.DrainQueuedBlocks(1).status !=
          olouie::audio::AudioEncodeSessionStatus::InvalidConfig ||
      null_session_bridge.DrainAllQueuedBlocks().status !=
          olouie::audio::AudioEncodeSessionStatus::InvalidConfig) {
    return Fail("Audio capture encode bridge should reject missing sessions.");
  }

  olouie::audio::AudioEncodeSessionBindingOptions small_options =
      binding_options;
  small_options.queue_capacity = 1;
  FakeAacEncoder full_encoder;
  FakeAacEncoder unused_encoder;
  const std::array full_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &full_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &unused_encoder},
  };
  std::vector<olouie::audio::AudioEncodeSessionTrack> full_tracks;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, small_options, format_slots, full_slots, &full_tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Capture encode bridge queue-full binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession full_session(full_tracks, &store);
  olouie::audio::AudioCaptureEncodeBridge full_bridge(plan, &full_session);
  if (!olouie::audio::DispatchCapturedPcm(full_bridge.captured_pcm_sink(),
                                          captured)
           .Succeeded()) {
    return Fail("Audio capture encode bridge queue-full setup failed.");
  }

  delivered =
      olouie::audio::DispatchCapturedPcm(full_bridge.captured_pcm_sink(),
                                         captured);
  if (delivered.status != olouie::audio::CapturedPcmSinkStatus::QueueError ||
      full_bridge.sink_stats().queue_error_count != 1 ||
      full_bridge.last_dispatch_result().queue_status !=
          olouie::audio::AudioEncodeSessionStatus::QueueError ||
      full_session.stats().queue_failure_count != 1) {
    return Fail("Audio capture encode bridge should surface queue errors.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioCaptureManagerBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.process_loopback_count = 2;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for capture manager failed: " << error
               << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 5 ||
      plan.tracks[0].kind != olouie::audio::AudioTrackKind::DefaultMixed) {
    return Fail("Audio capture manager test expected mixed plus sources.");
  }

  FakeCapturedPcmSink sink;
  olouie::audio::AudioCaptureManager manager;
  auto configured = manager.Configure(plan, &sink);
  if (!configured.Succeeded() || !manager.IsConfigured() ||
      manager.sink() != &sink || manager.source_count() != 4 ||
      manager.supported_source_count() != 2 ||
      manager.deferred_source_count() != 2 ||
      !manager.has_deferred_mixed_track()) {
    return Fail("Audio capture manager did not configure planned sources.");
  }

  const auto& sources = manager.sources();
  if (sources[0].source.kind !=
          olouie::audio::AudioTrackKind::SystemLoopback ||
      sources[0].track_id != plan.tracks[1].track_id ||
      sources[0].runtime !=
          olouie::audio::AudioCaptureSourceRuntime::SystemLoopback ||
      !olouie::audio::AudioCaptureSourceIsSupported(sources[0]) ||
      sources[0].sink != &sink) {
    return Fail("Audio capture manager did not bind system loopback.");
  }

  const auto* mic = manager.FindSource(
      {olouie::audio::AudioTrackKind::Microphone, 0});
  if (mic == nullptr ||
      mic->runtime != olouie::audio::AudioCaptureSourceRuntime::Microphone ||
      mic->support != olouie::audio::AudioCaptureSourceSupport::Supported ||
      mic->track_id != plan.tracks[2].track_id || mic->sink != &sink) {
    return Fail("Audio capture manager did not bind microphone.");
  }

  const auto* process = manager.FindSource(
      {olouie::audio::AudioTrackKind::ProcessLoopback, 1});
  if (process == nullptr ||
      process->runtime !=
          olouie::audio::AudioCaptureSourceRuntime::ProcessLoopback ||
      process->support != olouie::audio::AudioCaptureSourceSupport::Deferred ||
      olouie::audio::AudioCaptureSourceIsSupported(*process) ||
      process->track_id != plan.tracks[4].track_id ||
      process->sink != &sink) {
    return Fail("Audio capture manager should mark process loopback deferred.");
  }

  if (manager.FindSource({olouie::audio::AudioTrackKind::DefaultMixed, 0}) !=
          nullptr ||
      manager.FindSource({olouie::audio::AudioTrackKind::Microphone, 1}) !=
          nullptr) {
    return Fail("Audio capture manager should only expose captured sources.");
  }

  manager.Reset();
  if (manager.IsConfigured() || manager.source_count() != 0 ||
      manager.sink() != nullptr || manager.has_deferred_mixed_track()) {
    return Fail("Audio capture manager reset should clear configuration.");
  }

  configured = manager.Configure(plan, nullptr);
  if (configured.status !=
          olouie::audio::AudioCaptureManagerStatus::MissingSink ||
      manager.IsConfigured()) {
    return Fail("Audio capture manager should reject missing sinks.");
  }

  olouie::audio::AudioTrackPlan mixed_only_plan;
  options = {};
  options.microphone = true;
  options.separate_source_tracks = false;
  options.default_mixed_track = true;
  if (!olouie::audio::BuildAudioTrackPlan(options, &mixed_only_plan, &error)) {
    std::wcerr << L"Mixed-only track plan for capture manager failed: "
               << error << L'\n';
    return 1;
  }

  configured = manager.Configure(mixed_only_plan, &sink);
  if (configured.status !=
          olouie::audio::AudioCaptureManagerStatus::UnsupportedPlan ||
      manager.IsConfigured()) {
    return Fail("Audio capture manager should reject mixed-only plans.");
  }

  olouie::audio::AudioTrackPlan process_only_plan;
  options = {};
  options.system_loopback = false;
  options.default_mixed_track = false;
  options.process_loopback_count = 1;
  if (!olouie::audio::BuildAudioTrackPlan(options, &process_only_plan,
                                          &error)) {
    std::wcerr << L"Process-only track plan for capture manager failed: "
               << error << L'\n';
    return 1;
  }

  configured = manager.Configure(process_only_plan, &sink);
  if (!configured.Succeeded() || !manager.IsConfigured() ||
      manager.source_count() != 1 || manager.supported_source_count() != 0 ||
      manager.deferred_source_count() != 1 ||
      manager.has_deferred_mixed_track()) {
    return Fail("Audio capture manager should preserve deferred sources.");
  }

  olouie::audio::AudioTrackPlan invalid_packet_plan = plan;
  invalid_packet_plan.packet_tracks[1].track_id = 9999;
  configured = manager.Configure(invalid_packet_plan, &sink);
  if (configured.status !=
          olouie::audio::AudioCaptureManagerStatus::InvalidPlan ||
      manager.IsConfigured()) {
    return Fail("Audio capture manager should reject malformed packet tracks.");
  }

  olouie::audio::AudioTrackPlan duplicate_source_plan = plan;
  duplicate_source_plan.tracks.push_back(plan.tracks[1]);
  duplicate_source_plan.tracks.back().track_id = 99;
  duplicate_source_plan.packet_tracks.push_back(
      duplicate_source_plan.tracks.back().ToPacketTrack());
  configured = manager.Configure(duplicate_source_plan, &sink);
  if (configured.status !=
          olouie::audio::AudioCaptureManagerStatus::DuplicateSource ||
      manager.IsConfigured()) {
    return Fail("Audio capture manager should reject duplicate sources.");
  }

  olouie::audio::AudioTrackPlan invalid_source_plan = plan;
  invalid_source_plan.tracks[2].source_index = 1;
  configured = manager.Configure(invalid_source_plan, &sink);
  if (configured.status !=
          olouie::audio::AudioCaptureManagerStatus::InvalidPlan ||
      manager.IsConfigured()) {
    return Fail("Audio capture manager should reject invalid sources.");
  }

  return 0;
}

int VerifyAudioCaptureSmokeOrchestrationBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.process_loopback_count = 1;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for capture smoke failed: " << error
               << L'\n';
    return 1;
  }

  FakeCapturedPcmSink sink;
  olouie::audio::AudioCaptureSmokeResult smoke;
  ResetFakeAudioCaptureSmokeState();
  auto run = olouie::audio::RunAudioCaptureSmoke(
      plan, std::chrono::milliseconds(7), &sink,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (!run.Succeeded() || !smoke.deferred_mixed_track ||
      smoke.sources.size() != 3 || smoke.attempted_source_count != 2 ||
      smoke.succeeded_source_count != 2 || smoke.deferred_source_count != 1 ||
      smoke.packet_count != 2 || smoke.frame_count != 8 ||
      sink.packet_count != 2 ||
      g_audio_capture_smoke_state.system_call_count != 1 ||
      g_audio_capture_smoke_state.mic_call_count != 1 ||
      g_audio_capture_smoke_state.last_system_duration !=
          std::chrono::milliseconds(7) ||
      g_audio_capture_smoke_state.last_mic_duration !=
          std::chrono::milliseconds(7)) {
    return Fail("Audio capture smoke did not run supported planned sources.");
  }

  if (smoke.sources[0].source.kind !=
          olouie::audio::AudioTrackKind::SystemLoopback ||
      !smoke.sources[0].attempted || !smoke.sources[0].succeeded ||
      smoke.sources[0].capture.packet_count != 1 ||
      smoke.sources[1].source.kind !=
          olouie::audio::AudioTrackKind::Microphone ||
      !smoke.sources[1].attempted || !smoke.sources[1].succeeded ||
      smoke.sources[2].source.kind !=
          olouie::audio::AudioTrackKind::ProcessLoopback ||
      smoke.sources[2].attempted || smoke.sources[2].succeeded ||
      smoke.sources[2].support !=
          olouie::audio::AudioCaptureSourceSupport::Deferred) {
    return Fail("Audio capture smoke source results are incorrect.");
  }

  olouie::audio::AudioTrackPlan process_only_plan;
  options = {};
  options.system_loopback = false;
  options.default_mixed_track = false;
  options.process_loopback_count = 1;
  if (!olouie::audio::BuildAudioTrackPlan(options, &process_only_plan,
                                          &error)) {
    std::wcerr << L"Process-only track plan for capture smoke failed: "
               << error << L'\n';
    return 1;
  }

  ResetFakeAudioCaptureSmokeState();
  run = olouie::audio::RunAudioCaptureSmoke(
      process_only_plan, std::chrono::milliseconds(5), &sink,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (!run.Succeeded() || smoke.sources.size() != 1 ||
      smoke.attempted_source_count != 0 || smoke.succeeded_source_count != 0 ||
      smoke.deferred_source_count != 1 || smoke.packet_count != 0 ||
      g_audio_capture_smoke_state.system_call_count != 0 ||
      g_audio_capture_smoke_state.mic_call_count != 0) {
    return Fail("Audio capture smoke should skip deferred-only plans.");
  }

  ResetFakeAudioCaptureSmokeState();
  run = olouie::audio::RunAudioCaptureSmoke(
      plan, std::chrono::milliseconds(0), &sink,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status != olouie::audio::AudioCaptureSmokeStatus::InvalidConfig ||
      !smoke.sources.empty() ||
      g_audio_capture_smoke_state.system_call_count != 0 ||
      g_audio_capture_smoke_state.mic_call_count != 0) {
    return Fail("Audio capture smoke should reject invalid durations.");
  }

  run = olouie::audio::RunAudioCaptureSmoke(
      plan, std::chrono::milliseconds(5), nullptr,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status != olouie::audio::AudioCaptureSmokeStatus::InvalidConfig ||
      !smoke.sources.empty()) {
    return Fail("Audio capture smoke should reject missing sinks.");
  }

  olouie::audio::AudioCaptureSmokeRunners missing_runner =
      FakeAudioCaptureSmokeRunners();
  missing_runner.system_loopback = nullptr;
  run = olouie::audio::RunAudioCaptureSmoke(
      plan, std::chrono::milliseconds(5), &sink, missing_runner, &smoke);
  if (run.status != olouie::audio::AudioCaptureSmokeStatus::InvalidConfig ||
      smoke.attempted_source_count != 1 || smoke.sources.size() != 1 ||
      !smoke.sources[0].attempted || smoke.sources[0].succeeded) {
    return Fail("Audio capture smoke should reject missing source runners.");
  }

  ResetFakeAudioCaptureSmokeState();
  g_audio_capture_smoke_state.system_result = false;
  run = olouie::audio::RunAudioCaptureSmoke(
      plan, std::chrono::milliseconds(5), &sink,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status != olouie::audio::AudioCaptureSmokeStatus::SourceFailed ||
      smoke.attempted_source_count != 2 || smoke.succeeded_source_count != 1 ||
      g_audio_capture_smoke_state.system_call_count != 1 ||
      g_audio_capture_smoke_state.mic_call_count != 1 ||
      smoke.sources[0].succeeded || !smoke.sources[1].succeeded) {
    return Fail("Audio capture smoke should preserve partial failures.");
  }

  return 0;
}

int VerifyAudioCaptureEncodeSetupBoundary() {
  ResetFakeAudioCaptureEncodeSetupState();

  olouie::audio::AudioCaptureEncodePreflightOptions preflight_options;
  preflight_options.microphone = true;
  preflight_options.require_microphone = false;
  preflight_options.default_mixed_track = false;

  olouie::audio::AudioCaptureEncodePreflight preflight;
  auto setup_result = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &FakeAudioCaptureEncodeFormatProvider, &preflight);
  if (!setup_result.Succeeded() || !preflight.IsUsable() ||
      preflight.plan.tracks.size() != 2 ||
      preflight.format_slots.size() != 2 || preflight.tracks.size() != 2 ||
      preflight.sources.size() != 2 ||
      !preflight.sources[0].included || !preflight.sources[1].included ||
      preflight.plan.tracks[0].kind !=
          olouie::audio::AudioTrackKind::SystemLoopback ||
      preflight.plan.tracks[1].kind !=
          olouie::audio::AudioTrackKind::Microphone ||
      preflight.tracks[0].prepared_format.bits_per_sample != 16 ||
      preflight.tracks[1].prepared_format.channel_count != 1 ||
      g_audio_capture_encode_setup_state.format_query_count != 2) {
    return Fail("Audio capture encode preflight did not build direct tracks.");
  }

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieAudioCaptureEncodeSetupTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  std::wstring error;
  auto store =
      olouie::record::PacketStore::Create(session_dir,
                                          preflight.plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for setup boundary failed: " << error
               << L'\n';
    return 1;
  }

  olouie::audio::AudioCaptureEncodeSessionSetupOptions session_options;
  session_options.queue_capacity = 3;
  session_options.aac_bitrate_bps = 160000;

  olouie::audio::AudioCaptureEncodeSessionSetup setup;
  setup_result = olouie::audio::BuildAudioCaptureEncodeSessionSetup(
      preflight, session_options, &store,
      &FakeAudioCaptureEncodeEncoderFactory, &setup);
  if (!setup_result.Succeeded() || !setup.IsConfigured() ||
      setup.encoder_infos.size() != 2 || setup.encoders.size() != 2 ||
      setup.encoder_slots.size() != 2 || setup.session_tracks.size() != 2 ||
      setup.encoder_infos[0].backend_name != L"Fake AAC encoder" ||
      !setup.encoder_infos[0].output_metadata.IsReady() ||
      setup.encoder_infos[0].output_metadata.audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x90}) ||
      g_audio_capture_encode_setup_state.encoder_create_count != 2 ||
      g_audio_capture_encode_setup_state.encoder_track_ids[0] !=
          preflight.plan.tracks[0].track_id ||
      g_audio_capture_encode_setup_state.encoder_bitrates[1] != 160000) {
    return Fail("Audio capture encode session setup did not initialize.");
  }

  std::vector<olouie::record::AudioTrackSessionManifest> manifest_tracks;
  const auto metadata_result = olouie::audio::BuildAudioRecordingMetadata(
      preflight, setup, &manifest_tracks);
  if (!metadata_result.Succeeded() || manifest_tracks.size() != 2 ||
      manifest_tracks[0].track_id != preflight.plan.tracks[0].track_id ||
      manifest_tracks[0].source_kind != L"system_loopback" ||
      manifest_tracks[0].sample_rate != 48000 ||
      manifest_tracks[0].channel_count != 2 ||
      manifest_tracks[0].aac_audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x90}) ||
      manifest_tracks[1].source_kind != L"microphone" ||
      manifest_tracks[1].channel_count != 1 ||
      manifest_tracks[1].aac_audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x88})) {
    return Fail("Audio recording metadata did not preserve AAC track data.");
  }

  if (olouie::audio::BuildAudioRecordingMetadata(preflight, setup, nullptr)
          .status !=
      olouie::audio::AudioRecordingMetadataStatus::InvalidConfig) {
    return Fail("Audio recording metadata should require a destination.");
  }

  const uint32_t original_track_id = setup.encoder_infos[0].track_id;
  setup.encoder_infos[0].track_id = original_track_id + 100;
  if (olouie::audio::BuildAudioRecordingMetadata(
          preflight, setup, &manifest_tracks)
          .status !=
      olouie::audio::AudioRecordingMetadataStatus::MetadataMismatch) {
    return Fail("Audio recording metadata should reject track mismatches.");
  }
  setup.encoder_infos[0].track_id = original_track_id;

  auto* system_encoder =
      dynamic_cast<FakeAacEncoder*>(setup.encoders[0].get());
  if (system_encoder == nullptr) {
    return Fail("Audio capture encode setup did not keep fake encoders.");
  }

  olouie::audio::PcmPacketTiming timing;
  if (!olouie::audio::BuildPcmPacketTiming(128, 456, 4, 48000, &timing,
                                           &error)) {
    return 1;
  }
  const auto pcm_bytes =
      BytesFromFloats({-1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.75f, -0.25f, 0.25f});
  auto queued = setup.session->QueueCapturedPcm(
      preflight.plan.tracks[0].track_id,
      preflight.tracks[0].captured_format,
      olouie::audio::PcmPacketInfo{timing, false}, 45000,
      std::span<const std::byte>(pcm_bytes.data(), pcm_bytes.size()));
  auto drained = setup.session->DrainAllQueuedBlocks();
  if (!queued.Succeeded() || !drained.Succeeded() ||
      drained.processed_block_count != 1 || system_encoder->submit_count != 1 ||
      setup.session->stats().queued_packet_count != 1 ||
      setup.session->stats().drained_block_count != 1) {
    return Fail("Audio capture encode setup session should be usable.");
  }

  olouie::audio::AudioCaptureEncodePreflight optional_mic_preflight;
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.mic_available = false;
  setup_result = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &FakeAudioCaptureEncodeFormatProvider,
      &optional_mic_preflight);
  if (!setup_result.Succeeded() ||
      optional_mic_preflight.plan.tracks.size() != 1 ||
      optional_mic_preflight.sources.size() != 2 ||
      optional_mic_preflight.sources[1].included ||
      optional_mic_preflight.sources[1].message.empty()) {
    return Fail("Optional microphone preflight should defer unavailable mic.");
  }

  olouie::audio::AudioCaptureEncodePreflight required_mic_preflight;
  preflight_options.require_microphone = true;
  setup_result = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &FakeAudioCaptureEncodeFormatProvider,
      &required_mic_preflight);
  if (setup_result.status !=
      olouie::audio::AudioCaptureEncodeSetupStatus::SourceUnavailable) {
    return Fail("Required microphone preflight should fail when unavailable.");
  }
  preflight_options.require_microphone = false;

  olouie::audio::AudioCaptureEncodePreflight resample_preflight;
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.system_format =
      olouie::audio::MakePcmStreamFormat(
          44100, 2, 32, 8, 352800,
          olouie::audio::PcmSampleEncoding::Float);
  preflight_options.microphone = false;
  preflight_options.output_sample_rate = 48000;
  setup_result = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &FakeAudioCaptureEncodeFormatProvider,
      &resample_preflight);
  if (!setup_result.Succeeded() || !resample_preflight.IsUsable() ||
      resample_preflight.tracks.size() != 1 ||
      resample_preflight.tracks[0].captured_format.sample_rate != 44100 ||
      resample_preflight.tracks[0].prepared_format.sample_rate != 48000) {
    return Fail("Preflight should configure sample-rate conversion.");
  }
  olouie::audio::AudioCaptureEncodeSessionSetup resample_setup;
  setup_result = olouie::audio::BuildAudioCaptureEncodeSessionSetup(
      resample_preflight, session_options, &store,
      &FakeAudioCaptureEncodeEncoderFactory, &resample_setup);
  auto* resample_encoder =
      resample_setup.encoders.empty()
          ? nullptr
          : dynamic_cast<FakeAacEncoder*>(resample_setup.encoders[0].get());
  olouie::audio::PcmPacketTiming resample_timing;
  if (!setup_result.Succeeded() || !resample_setup.IsConfigured() ||
      resample_encoder == nullptr ||
      !olouie::audio::BuildPcmPacketTiming(
          0, 1000, 441, 44100, &resample_timing, &error)) {
    return Fail("Sample-rate conversion session setup failed.");
  }
  const std::vector<float> resample_samples(441 * 2, 0.125f);
  const auto resample_queued = resample_setup.session->QueueCapturedPcm(
      resample_preflight.plan.tracks[0].track_id,
      resample_preflight.tracks[0].captured_format,
      olouie::audio::PcmPacketInfo{resample_timing, false}, 100000,
      BytesFromFloats(resample_samples));
  const auto resample_drained =
      resample_setup.session->DrainAllQueuedBlocks();
  if (!resample_queued.Succeeded() || !resample_drained.Succeeded() ||
      resample_encoder->submit_count != 1 ||
      resample_encoder->last_input.frame_count != 480 ||
      resample_encoder->last_input.duration_ns != 10000000) {
    return Fail("Sample-rate conversion did not reach the AAC encoder.");
  }
  preflight_options.output_sample_rate = 0;

  olouie::audio::AudioCaptureEncodeSessionSetup failed_setup;
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.encoder_available = false;
  setup_result = olouie::audio::BuildAudioCaptureEncodeSessionSetup(
      preflight, session_options, &store,
      &FakeAudioCaptureEncodeEncoderFactory, &failed_setup);
  if (setup_result.status !=
          olouie::audio::AudioCaptureEncodeSetupStatus::EncoderInitFailed ||
      failed_setup.IsConfigured()) {
    return Fail("Session setup should surface unavailable AAC backends.");
  }

  olouie::audio::AudioCaptureEncodePreflight invalid_mixed_preflight;
  preflight_options.default_mixed_track = true;
  setup_result = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &FakeAudioCaptureEncodeFormatProvider,
      &invalid_mixed_preflight);
  if (setup_result.status !=
      olouie::audio::AudioCaptureEncodeSetupStatus::InvalidConfig) {
    return Fail("Preflight should keep default mixing deferred.");
  }

  if (std::wstring(olouie::audio::AudioCaptureEncodeSetupStatusName(
          olouie::audio::AudioCaptureEncodeSetupStatus::BindingFailed)) !=
      L"binding failed") {
    return Fail("Audio capture encode setup status names changed.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioRecordingSessionBoundary() {
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.mic_format =
      g_audio_capture_encode_setup_state.system_format;
  ResetFakeAudioLiveCaptureState();

  olouie::audio::AudioRecordingSessionOptions options;
  options.preflight.microphone = true;
  options.preflight.require_microphone = false;
  options.preflight.default_mixed_track = false;
  options.setup.queue_capacity = 4;
  options.setup.aac_bitrate_bps = 160000;
  options.live.duration = std::chrono::milliseconds(3);
  options.live.drain_interval = std::chrono::milliseconds(1);
  options.live.max_blocks_per_drain_tick = 1;
  options.live.qpc_origin_ns = 600;
  options.format_provider = &FakeAudioCaptureEncodeFormatProvider;
  options.encoder_factory = &FakeAudioCaptureEncodeEncoderFactory;
  options.live_source_factory = &FakeAudioLiveCaptureSourceFactory;

  olouie::audio::AudioRecordingSession invalid_order(options);
  auto result = invalid_order.RunForDuration(std::chrono::milliseconds(1));
  if (result.status != olouie::audio::AudioRecordingSessionStatus::InvalidState) {
    return Fail("Audio recording session should reject run before prepare.");
  }

  olouie::audio::AudioRecordingSession session(options);
  result = session.Preflight();
  if (!result.Succeeded() || !session.IsPreflighted() ||
      session.IsPrepared() || session.preflight().plan.tracks.size() != 2 ||
      g_audio_capture_encode_setup_state.format_query_count != 2) {
    return Fail("Audio recording session preflight did not build tracks.");
  }

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieAudioRecordingSessionTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  std::wstring error;
  auto store = olouie::record::PacketStore::Create(
      session_dir, session.preflight().plan.packet_tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for recording session failed: "
               << error << L'\n';
    return 1;
  }

  olouie::audio::AudioRecordingSession no_preflight(options);
  result = no_preflight.Prepare(&store);
  if (result.status != olouie::audio::AudioRecordingSessionStatus::InvalidState) {
    return Fail("Audio recording session should reject prepare before preflight.");
  }

  result = session.Prepare(&store);
  if (!result.Succeeded() || !session.IsPrepared() ||
      session.packet_store() != &store || session.encode_session() == nullptr ||
      session.setup().encoder_infos.size() != 2 ||
      g_audio_capture_encode_setup_state.encoder_create_count != 2 ||
      g_audio_capture_encode_setup_state.encoder_bitrates[0] != 160000) {
    return Fail("Audio recording session did not prepare encoders.");
  }

  auto* system_encoder =
      dynamic_cast<FakeAacEncoder*>(session.setup().encoders[0].get());
  auto* mic_encoder =
      dynamic_cast<FakeAacEncoder*>(session.setup().encoders[1].get());
  if (system_encoder == nullptr || mic_encoder == nullptr) {
    return Fail("Audio recording session did not keep fake encoder ownership.");
  }

  result = session.RunForDuration(std::chrono::milliseconds(3));
  const auto& live = session.last_live_result();
  if (!result.Succeeded() || !result.live_result.Succeeded() ||
      live.sources.size() != 2 || live.attempted_source_count != 2 ||
      live.started_source_count != 2 || live.deferred_source_count != 0 ||
      live.sink_stats.received_packet_count != 2 ||
      live.sink_stats.queued_packet_count != 2 ||
      live.drained_block_count != 2 ||
      !live.final_drain.Succeeded() || !live.flush.Succeeded() ||
      session.encode_session()->stats().queued_packet_count != 2 ||
      session.encode_session()->stats().drained_block_count != 2 ||
      session.encode_session()->stats().flushed_track_count != 2 ||
      system_encoder->submit_count != 1 || mic_encoder->submit_count != 1 ||
      system_encoder->flush_count != 1 || mic_encoder->flush_count != 1 ||
      system_encoder->last_input.pts_ns != 45000 ||
      mic_encoder->last_input.pts_ns != 55000 ||
      g_audio_live_capture_state.system_stop_count != 1 ||
      g_audio_live_capture_state.mic_stop_count != 1) {
    return Fail("Audio recording session did not run live capture encode.");
  }

  olouie::audio::AudioRecordingSessionOptions setup_fail_options = options;
  olouie::audio::AudioRecordingSession setup_fail(setup_fail_options);
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.mic_format =
      g_audio_capture_encode_setup_state.system_format;
  result = setup_fail.Preflight();
  if (!result.Succeeded()) {
    return Fail("Audio recording session setup-failure preflight failed.");
  }
  g_audio_capture_encode_setup_state.encoder_available = false;
  result = setup_fail.Prepare(&store);
  if (result.status !=
          olouie::audio::AudioRecordingSessionStatus::SetupFailed ||
      setup_fail.IsPrepared()) {
    return Fail("Audio recording session should surface setup failures.");
  }

  olouie::audio::AudioRecordingSessionOptions resample_options = options;
  resample_options.preflight.microphone = false;
  resample_options.preflight.output_sample_rate = 48000;
  ResetFakeAudioCaptureEncodeSetupState();
  g_audio_capture_encode_setup_state.system_format =
      olouie::audio::MakePcmStreamFormat(
          44100, 2, 32, 8, 352800,
          olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioRecordingSession resample_session(resample_options);
  result = resample_session.Preflight();
  if (!result.Succeeded() || !resample_session.IsPreflighted() ||
      resample_session.preflight().tracks.size() != 1 ||
      resample_session.preflight().tracks[0].captured_format.sample_rate !=
          44100 ||
      resample_session.preflight().tracks[0].prepared_format.sample_rate !=
          48000) {
    return Fail("Audio recording session should accept mixed sample rates.");
  }

  olouie::audio::AudioRecordingSessionOptions mixed_options = options;
  mixed_options.preflight.default_mixed_track = true;
  olouie::audio::AudioRecordingSession mixed_session(mixed_options);
  result = mixed_session.Preflight();
  if (result.status !=
      olouie::audio::AudioRecordingSessionStatus::InvalidConfig) {
    return Fail("Audio recording session should keep mixed track deferred.");
  }

  if (std::wstring(olouie::audio::AudioRecordingSessionStatusName(
          olouie::audio::AudioRecordingSessionStatus::RunFailed)) !=
      L"run failed") {
    return Fail("Audio recording session status names changed.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioCaptureEncodeSmokeBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.process_loopback_count = 1;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for capture encode smoke failed: "
               << error << L'\n';
    return 1;
  }

  if (plan.tracks.size() != 3) {
    return Fail("Audio capture encode smoke expected three direct tracks.");
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 3;
  const auto format_slots = FormatSlotsForPlan(plan, float_format);

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  FakeAacEncoder process_encoder;
  const std::array slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &mic_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[2].track_id, &process_encoder},
  };

  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, slots, &tracks);
  if (!binding.Succeeded()) {
    std::wcerr << L"Capture encode smoke binding failed: "
               << binding.message << L'\n';
    return 1;
  }

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieAudioCaptureEncodeSmokeTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for capture encode smoke failed: "
               << error << L'\n';
    return 1;
  }

  olouie::audio::AudioEncodeSession session(tracks, &store);
  olouie::audio::AudioCaptureEncodeSmokeResult smoke;
  ResetFakeAudioCaptureSmokeState();
  auto run = olouie::audio::RunAudioCaptureEncodeSmoke(
      plan, std::chrono::milliseconds(7), &session, 600,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (!run.Succeeded() || !smoke.bridge_configured ||
      smoke.capture.attempted_source_count != 2 ||
      smoke.capture.succeeded_source_count != 2 ||
      smoke.capture.deferred_source_count != 1 ||
      !smoke.drain.Succeeded() || smoke.drain.processed_block_count != 2 ||
      smoke.sink_stats.received_packet_count != 2 ||
      smoke.sink_stats.queued_packet_count != 2 ||
      session.stats().queued_packet_count != 2 ||
      session.stats().drained_block_count != 2 ||
      system_encoder.submit_count != 1 || mic_encoder.submit_count != 1 ||
      process_encoder.submit_count != 0 ||
      system_encoder.last_input.pts_ns != 45000 ||
      mic_encoder.last_input.pts_ns != 55000 ||
      system_encoder.last_pcm_size != 16 || mic_encoder.last_pcm_size != 16) {
    return Fail("Audio capture encode smoke did not capture and drain PCM.");
  }

  olouie::audio::AudioEncodeSession null_session({}, &store);
  run = olouie::audio::RunAudioCaptureEncodeSmoke(
      plan, std::chrono::milliseconds(7), &null_session, 0,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status !=
          olouie::audio::AudioCaptureEncodeSmokeStatus::InvalidConfig ||
      smoke.bridge_configured) {
    return Fail("Audio capture encode smoke should reject bad sessions.");
  }

  FakeAacEncoder capture_fail_system_encoder;
  FakeAacEncoder capture_fail_mic_encoder;
  FakeAacEncoder capture_fail_process_encoder;
  const std::array capture_fail_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &capture_fail_system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &capture_fail_mic_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[2].track_id, &capture_fail_process_encoder},
  };
  std::vector<olouie::audio::AudioEncodeSessionTrack> capture_fail_tracks;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, capture_fail_slots,
      &capture_fail_tracks);
  if (!binding.Succeeded()) {
    return 1;
  }

  olouie::audio::AudioEncodeSession capture_fail_session(
      capture_fail_tracks, &store);
  ResetFakeAudioCaptureSmokeState();
  g_audio_capture_smoke_state.system_result = false;
  run = olouie::audio::RunAudioCaptureEncodeSmoke(
      plan, std::chrono::milliseconds(7), &capture_fail_session, 600,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status !=
          olouie::audio::AudioCaptureEncodeSmokeStatus::CaptureFailed ||
      !smoke.drain.Succeeded() || smoke.drain.processed_block_count != 1 ||
      capture_fail_system_encoder.submit_count != 0 ||
      capture_fail_mic_encoder.submit_count != 1 ||
      capture_fail_mic_encoder.last_input.pts_ns != 55000) {
    return Fail("Audio capture encode smoke should drain partial captures.");
  }

  FakeAacEncoder drain_fail_system_encoder;
  FakeAacEncoder drain_fail_mic_encoder;
  FakeAacEncoder drain_fail_process_encoder;
  drain_fail_system_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  const std::array drain_fail_slots{
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[0].track_id, &drain_fail_system_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[1].track_id, &drain_fail_mic_encoder},
      olouie::audio::AudioEncodeSessionEncoderSlot{
          plan.tracks[2].track_id, &drain_fail_process_encoder},
  };
  std::vector<olouie::audio::AudioEncodeSessionTrack> drain_fail_tracks;
  binding = olouie::audio::BuildAudioEncodeSessionTracks(
      plan, binding_options, format_slots, drain_fail_slots,
      &drain_fail_tracks);
  if (!binding.Succeeded()) {
    return 1;
  }

  olouie::audio::AudioEncodeSession drain_fail_session(drain_fail_tracks,
                                                       &store);
  ResetFakeAudioCaptureSmokeState();
  run = olouie::audio::RunAudioCaptureEncodeSmoke(
      plan, std::chrono::milliseconds(7), &drain_fail_session, 600,
      FakeAudioCaptureSmokeRunners(), &smoke);
  if (run.status != olouie::audio::AudioCaptureEncodeSmokeStatus::DrainFailed ||
      smoke.drain.status != olouie::audio::AudioEncodeSessionStatus::DrainError ||
      drain_fail_system_encoder.submit_count != 1 ||
      drain_fail_mic_encoder.submit_count != 0 ||
      drain_fail_session.stats().drain_failure_count != 1) {
    return Fail("Audio capture encode smoke should surface drain failures.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyAudioLiveCaptureEncodeBoundary() {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  options.process_loopback_count = 1;
  options.default_mixed_track = false;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for live capture encode failed: "
               << error << L'\n';
    return 1;
  }

  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::AudioEncodeSessionBindingOptions binding_options;
  binding_options.queue_capacity = 4;
  const auto format_slots = FormatSlotsForPlan(plan, float_format);

  const auto root =
      std::filesystem::temp_directory_path() /
      L"O'LouieAudioLiveCaptureEncodeTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto store =
      olouie::record::PacketStore::Create(session_dir, plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for live capture encode failed: "
               << error << L'\n';
    return 1;
  }

  auto build_tracks =
      [&](FakeAacEncoder& system_encoder, FakeAacEncoder& mic_encoder,
          FakeAacEncoder& process_encoder,
          std::vector<olouie::audio::AudioEncodeSessionTrack>* tracks) {
        const std::array slots{
            olouie::audio::AudioEncodeSessionEncoderSlot{
                plan.tracks[0].track_id, &system_encoder},
            olouie::audio::AudioEncodeSessionEncoderSlot{
                plan.tracks[1].track_id, &mic_encoder},
            olouie::audio::AudioEncodeSessionEncoderSlot{
                plan.tracks[2].track_id, &process_encoder},
        };
        const auto binding = olouie::audio::BuildAudioEncodeSessionTracks(
            plan, binding_options, format_slots, slots, tracks);
        if (!binding.Succeeded()) {
          std::wcerr << L"Live capture encode binding failed: "
                     << binding.message << L'\n';
          return false;
        }
        return true;
      };

  olouie::audio::AudioLiveCaptureEncodeOptions live_options;
  live_options.duration = std::chrono::milliseconds(3);

  live_options.drain_interval = std::chrono::milliseconds(1);
  live_options.max_blocks_per_drain_tick = 1;
  live_options.qpc_origin_ns = 600;

  FakeAacEncoder system_encoder;
  FakeAacEncoder mic_encoder;
  FakeAacEncoder process_encoder;
  std::vector<olouie::audio::AudioEncodeSessionTrack> tracks;
  if (!build_tracks(system_encoder, mic_encoder, process_encoder, &tracks)) {
    return 1;
  }

  olouie::audio::AudioEncodeSession session(tracks, &store);
  auto invalid_performance_options = live_options;
  invalid_performance_options.performance_mode =
      static_cast<olouie::performance::CapturePerformanceMode>(99);
  olouie::audio::AudioLiveCaptureEncodeResult invalid_performance_live;
  auto invalid_performance = olouie::audio::RunAudioLiveCaptureEncode(
      plan, invalid_performance_options, &session,
      &FakeAudioLiveCaptureSourceFactory, &invalid_performance_live);
  if (invalid_performance.status !=
          olouie::audio::AudioLiveCaptureEncodeStatus::InvalidConfig ||
      !invalid_performance_live.sources.empty()) {
    return Fail("Live audio should reject an invalid performance mode.");
  }
  olouie::audio::AudioLiveCaptureEncodeResult live;
  ResetFakeAudioLiveCaptureState();
  auto run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, live_options, &session, &FakeAudioLiveCaptureSourceFactory, &live);
  if (!run.Succeeded() || !live.bridge_configured ||
      live.sources.size() != 3 || live.attempted_source_count != 2 ||
      live.started_source_count != 2 || live.deferred_source_count != 1 ||
      live.packet_count != 2 || live.frame_count != 8 ||
      live.sink_stats.received_packet_count != 2 ||
      live.sink_stats.queued_packet_count != 2 ||
      !live.final_drain.Succeeded() || !live.flush.Succeeded() ||
      live.drained_block_count != 2 ||
      session.stats().queued_packet_count != 2 ||
      session.stats().drained_block_count != 2 ||
      session.stats().flushed_track_count != 3 ||
      system_encoder.submit_count != 1 || mic_encoder.submit_count != 1 ||
      process_encoder.submit_count != 0 ||
      system_encoder.flush_count != 1 || mic_encoder.flush_count != 1 ||
      process_encoder.flush_count != 1 ||
      system_encoder.last_input.pts_ns != 45000 ||
      mic_encoder.last_input.pts_ns != 55000 ||
      g_audio_live_capture_state.system_stop_count != 1 ||
      g_audio_live_capture_state.mic_stop_count != 1) {
    return Fail("Live capture encode did not start, drain, and flush PCM.");
  }

  if (!live.sources[0].started || !live.sources[0].stopped ||
      !live.sources[1].started || !live.sources[1].stopped ||
      live.sources[2].attempted ||
      live.sources[2].support !=
          olouie::audio::AudioCaptureSourceSupport::Deferred) {
    return Fail("Live capture encode source results are incorrect.");
  }

  FakeAacEncoder continuity_system_encoder;
  FakeAacEncoder continuity_mic_encoder;
  FakeAacEncoder continuity_process_encoder;
  std::vector<olouie::audio::AudioEncodeSessionTrack> continuity_tracks;
  if (!build_tracks(continuity_system_encoder, continuity_mic_encoder,
                    continuity_process_encoder, &continuity_tracks)) {
    return 1;
  }
  olouie::audio::AudioEncodeSession continuity_session(continuity_tracks,
                                                        &store);
  olouie::record::SessionClock continuity_clock;
  if (!olouie::record::CaptureSessionClock(&continuity_clock, &error)) {
    std::wcerr << L"Continuity test clock failed: " << error << L'\n';
    return 1;
  }
  auto continuity_options = live_options;
  continuity_options.qpc_origin_ns = continuity_clock.origin_ns;
  continuity_options.maintain_track_continuity = true;
  ResetFakeAudioLiveCaptureState();
  g_audio_live_capture_state.system_emit_packet = false;
  g_audio_live_capture_state.mic_emit_packet = false;
  run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, continuity_options, &continuity_session,
      &FakeAudioLiveCaptureSourceFactory, &live);
  if (!run.Succeeded() || live.packet_count != 0 ||
      live.sources[0].synthetic_silence_packet_count == 0 ||
      live.sources[0].synthetic_silence_frame_count == 0 ||
      live.sources[1].synthetic_silence_packet_count == 0 ||
      live.sources[1].synthetic_silence_frame_count == 0 ||
      live.sink_stats.queued_packet_count < 2 ||
      continuity_system_encoder.submit_count == 0 ||
      continuity_mic_encoder.submit_count == 0 ||
      continuity_process_encoder.submit_count != 0) {
    return Fail("Live capture continuity should keep idle source tracks alive.");
  }

  FakeAacEncoder gap_system_encoder;
  FakeAacEncoder gap_mic_encoder;
  FakeAacEncoder gap_process_encoder;
  std::vector<olouie::audio::AudioEncodeSessionTrack> gap_tracks;
  if (!build_tracks(gap_system_encoder, gap_mic_encoder,
                    gap_process_encoder, &gap_tracks)) {
    return 1;
  }
  olouie::audio::AudioEncodeSession gap_session(gap_tracks, &store);
  if (!olouie::record::CaptureSessionClock(&continuity_clock, &error)) {
    std::wcerr << L"Gap test clock failed: " << error << L'\n';
    return 1;
  }
  continuity_options.qpc_origin_ns = continuity_clock.origin_ns;
  ResetFakeAudioLiveCaptureState();
  g_audio_live_capture_state.system_qpc_position_100ns =
      static_cast<uint64_t>((continuity_clock.origin_ns + 150000000) / 100);
  g_audio_live_capture_state.mic_qpc_position_100ns =
      static_cast<uint64_t>((continuity_clock.origin_ns + 150000000) / 100);
  g_audio_live_capture_state.system_data_discontinuity = true;
  g_audio_live_capture_state.mic_data_discontinuity = true;
  run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, continuity_options, &gap_session,
      &FakeAudioLiveCaptureSourceFactory, &live);
  if (!run.Succeeded() || live.packet_count != 2 ||
      live.sources[0].capture.data_discontinuity_count != 1 ||
      live.sources[1].capture.data_discontinuity_count != 1 ||
      live.sources[0].synthetic_silence_packet_count < 2 ||
      live.sources[1].synthetic_silence_packet_count < 2 ||
      live.sources[0].retimed_packet_count != 0 ||
      live.sources[1].retimed_packet_count != 0 ||
      gap_system_encoder.submit_count < 3 ||
      gap_mic_encoder.submit_count < 3) {
    return Fail("Live capture continuity should fill timestamped source gaps.");
  }

  FakeAacEncoder owned_system_encoder;
  FakeAacEncoder owned_mic_encoder;
  FakeAacEncoder owned_process_encoder;
  std::vector<olouie::audio::AudioEncodeSessionTrack> owned_tracks;
  if (!build_tracks(owned_system_encoder, owned_mic_encoder,
                    owned_process_encoder, &owned_tracks)) {
    return 1;
  }
  olouie::audio::AudioEncodeSession owned_encode_session(owned_tracks,
                                                         &store);
  ResetFakeAudioLiveCaptureState();
  owned_system_encoder.events = &g_audio_live_capture_state.events;
  owned_system_encoder.event_label = "system";
  owned_mic_encoder.events = &g_audio_live_capture_state.events;
  owned_mic_encoder.event_label = "mic";
  owned_process_encoder.events = &g_audio_live_capture_state.events;
  owned_process_encoder.event_label = "process";

  auto owned_options = live_options;
  owned_options.duration = std::chrono::milliseconds(0);
  olouie::audio::AudioLiveCaptureEncodeSession owned_live(
      plan, owned_options, &owned_encode_session,
      &FakeAudioLiveCaptureSourceFactory);
  const auto owned_prepared = owned_live.Prepare();
  const auto owned_started = owned_live.Start();
  const auto& owned_running_result = owned_live.result();
  const bool owned_live_stats_ready =
      owned_running_result.packet_count == 2 &&
      owned_running_result.frame_count == 8;
  const auto owned_stopped = owned_live.StopSources();
  const auto owned_drained = owned_live.DrainQueuedBlocks();
  const auto owned_flushed = owned_live.FlushEncoders();
  const std::vector<std::string> expected_owned_order{
      "start:system", "start:mic",   "stop:system",  "stop:mic",
      "submit:system", "drain:system", "submit:mic", "drain:mic",
      "flush:system", "flush:mic", "flush:process",
  };
  if (!owned_prepared.Succeeded() || !owned_started.Succeeded() ||
      !owned_live_stats_ready ||
      !owned_stopped.Succeeded() || !owned_drained.Succeeded() ||
      !owned_flushed.Succeeded() || owned_live.IsRunning() ||
      g_audio_live_capture_state.events != expected_owned_order) {
    return Fail("Owned live audio should stop inputs before drain and flush.");
  }

  olouie::audio::AudioLiveCaptureEncodeResult invalid_live;
  live_options.duration = std::chrono::milliseconds(0);
  run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, live_options, &session, &FakeAudioLiveCaptureSourceFactory,
      &invalid_live);
  if (run.status != olouie::audio::AudioLiveCaptureEncodeStatus::InvalidConfig ||
      !invalid_live.sources.empty()) {
    return Fail("Live capture encode should reject invalid options.");
  }
  live_options.duration = std::chrono::milliseconds(3);

  FakeAacEncoder start_fail_system_encoder;
  FakeAacEncoder start_fail_mic_encoder;
  FakeAacEncoder start_fail_process_encoder;
  std::vector<olouie::audio::AudioEncodeSessionTrack> start_fail_tracks;
  if (!build_tracks(start_fail_system_encoder, start_fail_mic_encoder,
                    start_fail_process_encoder, &start_fail_tracks)) {
    return 1;
  }

  olouie::audio::AudioEncodeSession start_fail_session(start_fail_tracks,
                                                       &store);
  ResetFakeAudioLiveCaptureState();
  g_audio_live_capture_state.mic_start_result = false;
  run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, live_options, &start_fail_session,
      &FakeAudioLiveCaptureSourceFactory, &live);
  if (run.status !=
          olouie::audio::AudioLiveCaptureEncodeStatus::SourceStartFailed ||
      live.attempted_source_count != 2 || live.started_source_count != 1 ||
      live.sink_stats.queued_packet_count != 1 ||
      start_fail_system_encoder.submit_count != 1 ||
      start_fail_mic_encoder.submit_count != 0 ||
      start_fail_session.stats().flushed_track_count != 3 ||
      g_audio_live_capture_state.system_stop_count != 1 ||
      g_audio_live_capture_state.mic_stop_count != 0) {
    return Fail("Live capture encode should stop and drain partial starts.");
  }

  FakeAacEncoder drain_fail_system_encoder;
  FakeAacEncoder drain_fail_mic_encoder;
  FakeAacEncoder drain_fail_process_encoder;
  drain_fail_system_encoder.submit_result =
      AacResult(olouie::audio::AacEncoderStatus::BackendError);
  std::vector<olouie::audio::AudioEncodeSessionTrack> drain_fail_tracks;
  if (!build_tracks(drain_fail_system_encoder, drain_fail_mic_encoder,
                    drain_fail_process_encoder, &drain_fail_tracks)) {
    return 1;
  }

  olouie::audio::AudioEncodeSession drain_fail_session(drain_fail_tracks,
                                                       &store);
  ResetFakeAudioLiveCaptureState();
  run = olouie::audio::RunAudioLiveCaptureEncode(
      plan, live_options, &drain_fail_session,
      &FakeAudioLiveCaptureSourceFactory, &live);
  if (run.status != olouie::audio::AudioLiveCaptureEncodeStatus::DrainFailed ||
      live.last_tick_drain.status !=
          olouie::audio::AudioEncodeSessionStatus::DrainError ||
      drain_fail_system_encoder.submit_count != 1 ||
      drain_fail_mic_encoder.submit_count != 1 ||
      drain_fail_session.stats().drain_failure_count != 1) {
    return Fail("Live capture encode should surface bounded drain failures.");
  }

  if (std::wstring(olouie::audio::AudioLiveCaptureEncodeStatusName(
          olouie::audio::AudioLiveCaptureEncodeStatus::FlushFailed)) !=
      L"flush failed") {
    return Fail("Live capture encode status names changed unexpectedly.");
  }

  store.Close();
  std::filesystem::remove_all(root);
  return 0;
}

int VerifyWasapiCaptureSourceLifecycleBoundary() {
  FakeCapturedPcmSink sink;

  olouie::audio::WasapiCaptureSource system_source(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0});
  if (system_source.IsRunning() ||
      system_source.source().kind !=
          olouie::audio::AudioTrackKind::SystemLoopback ||
      system_source.SnapshotStats().HasPackets()) {
    return Fail("WASAPI live source should start in a stopped state.");
  }

  auto started = system_source.Start(nullptr);
  if (started.status !=
          olouie::audio::WasapiCaptureSourceStatus::InvalidConfig ||
      system_source.IsRunning()) {
    return Fail("WASAPI live source should reject a missing sink.");
  }

  if (!olouie::audio::detail::IsWasapiCaptureFailureRecoverable(
          AUDCLNT_E_DEVICE_INVALIDATED) ||
      !olouie::audio::detail::IsWasapiCaptureFailureRecoverable(
          AUDCLNT_E_SERVICE_NOT_RUNNING) ||
      olouie::audio::detail::IsWasapiCaptureFailureRecoverable(E_INVALIDARG) ||
      olouie::audio::detail::WasapiCaptureRecoveryDelayMs(0) != 100 ||
      olouie::audio::detail::WasapiCaptureRecoveryDelayMs(1) != 200 ||
      olouie::audio::detail::WasapiCaptureRecoveryDelayMs(5) != 2000 ||
      olouie::audio::detail::WasapiCaptureRecoveryDelayMs(100) != 2000) {
    return Fail("WASAPI capture recovery policy is incorrect.");
  }

  olouie::audio::WasapiCaptureSource invalid_index(
      {olouie::audio::AudioTrackKind::SystemLoopback, 1});
  started = invalid_index.Start(&sink);
  if (started.status !=
          olouie::audio::WasapiCaptureSourceStatus::InvalidConfig ||
      invalid_index.IsRunning()) {
    return Fail("WASAPI live source should reject invalid source indexes.");
  }

  olouie::audio::WasapiCaptureSource invalid_performance(
      {olouie::audio::AudioTrackKind::SystemLoopback, 0},
      static_cast<olouie::performance::CapturePerformanceMode>(99));
  started = invalid_performance.Start(&sink);
  if (started.status !=
          olouie::audio::WasapiCaptureSourceStatus::InvalidConfig ||
      invalid_performance.IsRunning()) {
    return Fail("WASAPI live source should reject invalid performance mode.");
  }

  olouie::audio::WasapiCaptureSource default_mixed(
      {olouie::audio::AudioTrackKind::DefaultMixed, 0});
  started = default_mixed.Start(&sink);
  if (started.status !=
          olouie::audio::WasapiCaptureSourceStatus::InvalidConfig ||
      default_mixed.IsRunning()) {
    return Fail("WASAPI live source should reject mixer-output sources.");
  }

  if (std::wstring(olouie::audio::WasapiCaptureSourceStatusName(
          olouie::audio::WasapiCaptureSourceStatus::SinkError)) !=
      L"sink error") {
    return Fail("WASAPI live source status names changed unexpectedly.");
  }

  system_source.Stop();
  if (system_source.IsRunning()) {
    return Fail("Stopping an idle WASAPI live source should remain stopped.");
  }

  return 0;
}

struct FakeMicMonitorBackendState {
  std::mutex mutex;
  std::condition_variable condition;
  olouie::audio::MicMonitorOptions options;
  bool entered = false;
  bool fail = false;
};

class FakeMicMonitorBackend final
    : public olouie::audio::IMicMonitorBackend {
 public:
  explicit FakeMicMonitorBackend(
      std::shared_ptr<FakeMicMonitorBackendState> state)
      : state_(std::move(state)) {}

  olouie::audio::MicMonitorBackendResult Run(
      const olouie::audio::MicMonitorOptions& options,
      const std::atomic_bool& stop_requested,
      UpdateSink update_sink) override {
    {
      std::lock_guard lock(state_->mutex);
      state_->options = options;
      state_->entered = true;
    }
    state_->condition.notify_all();
    if (state_->fail) {
      return {olouie::audio::MicMonitorBackendStatus::Failed,
              L"No output devices are available."};
    }

    olouie::audio::MicMonitorBackendUpdate update;
    update.monitoring_started = true;
    update.active_output_device_id = L"active-output";
    update.active_output_device_name = L"Test Headphones";
    update.using_fallback_output = true;
    update.peak_dbfs = -9.5f;
    update.clipping = false;
    update.underrun_count = 2;
    update.overflow_count = 1;
    update.queued_frame_count = 2400;
    update.queue_capacity_frames = 12000;
    update.message = L"Using Windows Default fallback.";
    update_sink(update);

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {olouie::audio::MicMonitorBackendStatus::Stopped,
            L"Microphone check stopped."};
  }

 private:
  std::shared_ptr<FakeMicMonitorBackendState> state_;
};

bool WaitForMicMonitorState(olouie::audio::MicMonitorSession* session,
                            olouie::audio::MicMonitorState expected) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (session->Snapshot().state == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

int VerifyMicMonitorBoundary() {
  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 1, 32, 4, 192000,
      olouie::audio::PcmSampleEncoding::Float);
  const auto float_bytes = BytesFromFloats({-0.5f, 0.25f, 0.0f});
  const float float_peak = olouie::audio::MeasurePcmPeakDbfs(
      float_format, float_bytes, false);
  if (float_peak < -6.1f || float_peak > -6.0f) {
    return Fail("Mic monitor float peak calculation is incorrect.");
  }

  const auto s16_format = olouie::audio::MakePcmStreamFormat(
      48000, 1, 16, 2, 96000,
      olouie::audio::PcmSampleEncoding::SignedInteger);
  const auto s16_bytes = BytesFromSigned16({0, 32767, -32768});
  if (olouie::audio::MeasurePcmPeakDbfs(s16_format, s16_bytes, false) !=
          0.0f ||
      olouie::audio::MeasurePcmPeakDbfs(s16_format, {}, true) != -60.0f) {
    return Fail("Mic monitor integer or silent peak calculation is incorrect.");
  }

  olouie::audio::detail::MicMonitorPcmFifo fifo(4, 2);
  const std::vector<std::byte> first_frames{
      std::byte{1}, std::byte{2}, std::byte{3},
      std::byte{4}, std::byte{5}, std::byte{6}};
  if (fifo.Push(first_frames.data(), 3, false) || fifo.size_frames() != 3) {
    return Fail("Mic monitor FIFO rejected an in-capacity push.");
  }
  std::vector<std::byte> popped(8, std::byte{0});
  if (fifo.Pop(popped.data(), 2) != 2 || popped[0] != std::byte{1} ||
      popped[3] != std::byte{4}) {
    return Fail("Mic monitor FIFO did not preserve FIFO order.");
  }
  const std::vector<std::byte> wrapped_frames{
      std::byte{7},  std::byte{8},  std::byte{9},
      std::byte{10}, std::byte{11}, std::byte{12}};
  if (fifo.Push(wrapped_frames.data(), 3, false) ||
      fifo.Pop(popped.data(), 8) != 4 || popped[0] != std::byte{5} ||
      popped[1] != std::byte{6} || popped[2] != std::byte{7} ||
      popped[7] != std::byte{12} || fifo.Pop(popped.data(), 1) != 0) {
    return Fail("Mic monitor FIFO wraparound or underrun behavior is incorrect.");
  }
  const std::vector<std::byte> overflowing_frames{
      std::byte{1}, std::byte{1}, std::byte{2}, std::byte{2},
      std::byte{3}, std::byte{3}, std::byte{4}, std::byte{4},
      std::byte{5}, std::byte{5}};
  if (!fifo.Push(overflowing_frames.data(), 5, false) ||
      fifo.size_frames() != fifo.capacity_frames() ||
      fifo.Pop(popped.data(), 4) != 4 || popped[0] != std::byte{2} ||
      popped[7] != std::byte{5}) {
    return Fail("Mic monitor FIFO did not drop the oldest overflow frames.");
  }
  if (fifo.Push(nullptr, 2, true) || fifo.Pop(popped.data(), 2) != 2 ||
      popped[0] != std::byte{0} || popped[3] != std::byte{0}) {
    return Fail("Mic monitor FIFO did not preserve captured silence.");
  }

  auto backend_state = std::make_shared<FakeMicMonitorBackendState>();
  olouie::audio::MicMonitorSession session([backend_state] {
    return std::make_unique<FakeMicMonitorBackend>(backend_state);
  });
  std::atomic_uint32_t state_notification_count = 0;
  session.SetStateSink(
      [&state_notification_count](const olouie::audio::MicMonitorSnapshot&) {
        ++state_notification_count;
      });

  olouie::audio::MicMonitorOptions options;
  options.output_device_id = L"saved-output";
  auto command = session.Start(options);
  if (!command.Accepted() ||
      !WaitForMicMonitorState(&session,
                              olouie::audio::MicMonitorState::Monitoring)) {
    return Fail("Mic monitor session did not enter monitoring state.");
  }
  command = session.Start(options);
  if (command.status !=
      olouie::audio::MicMonitorCommandStatus::AlreadyRunning) {
    return Fail("Mic monitor session should reject repeated starts.");
  }

  const auto monitoring = session.Snapshot();
  if (monitoring.requested_output_device_id != L"saved-output" ||
      monitoring.active_output_device_id != L"active-output" ||
      monitoring.active_output_device_name != L"Test Headphones" ||
      !monitoring.using_fallback_output || monitoring.peak_dbfs != -9.5f ||
      monitoring.underrun_count != 2 || monitoring.overflow_count != 1 ||
      monitoring.queued_frame_count != 2400 ||
      monitoring.queue_capacity_frames != 12000) {
    return Fail("Mic monitor snapshot did not preserve backend state.");
  }
  {
    std::lock_guard lock(backend_state->mutex);
    if (backend_state->options.output_device_id != L"saved-output") {
      return Fail("Mic monitor did not pass the selected output to its backend.");
    }
  }

  command = session.Stop();
  if (!command.Accepted() ||
      !WaitForMicMonitorState(&session,
                              olouie::audio::MicMonitorState::Idle)) {
    return Fail("Mic monitor session did not stop asynchronously.");
  }

  backend_state->fail = true;
  backend_state->entered = false;
  command = session.Start({});
  if (!command.Accepted() ||
      !WaitForMicMonitorState(&session,
                              olouie::audio::MicMonitorState::Failed) ||
      session.Snapshot().message.find(L"No output devices") ==
          std::wstring::npos) {
    return Fail("Mic monitor session did not surface backend failure.");
  }
  if (state_notification_count.load() < 4 ||
      std::wstring(olouie::audio::MicMonitorStateName(
          olouie::audio::MicMonitorState::Monitoring)) != L"monitoring" ||
      std::wstring(olouie::audio::MicMonitorCommandStatusName(
          olouie::audio::MicMonitorCommandStatus::AlreadyRunning)) !=
          L"already running") {
    return Fail("Mic monitor state notifications or names are incomplete.");
  }

  session.Shutdown();
  return 0;
}

int VerifyAudioResamplerBoundary() {
  const auto float_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 32, 8, 384000, olouie::audio::PcmSampleEncoding::Float);
  const std::vector<float> float_samples{
      -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.25f, -2.0f, 0.25f};

  olouie::audio::PreparedPcmBuffer prepared;
  auto result = olouie::audio::PreparePcmForAac(
      float_format, 4, BytesFromFloats(float_samples), 48000, &prepared);
  if (!result.Succeeded() || !prepared.IsValid()) {
    std::wcerr << L"Float PCM preparation failed: " << result.message << L'\n';
    return 1;
  }

  if (prepared.format.sample_rate != 48000 ||
      prepared.format.channel_count != 2 ||
      prepared.format.bits_per_sample != 16 ||
      prepared.format.block_align != 4 ||
      prepared.format.average_bytes_per_second != 192000 ||
      prepared.format.encoding != olouie::audio::PcmSampleEncoding::SignedInteger ||
      prepared.frame_count != 4 || prepared.data.size() != 16) {
    return Fail("Prepared PCM format shape is incorrect.");
  }

  const std::vector<int16_t> expected_samples{
      -32768, -16384, 0, 16384, 32767, 32767, -32768, 8192};
  for (size_t index = 0; index < expected_samples.size(); ++index) {
    if (ReadSigned16Sample(prepared.data, index) != expected_samples[index]) {
      return Fail("Float-to-S16 conversion produced an unexpected sample.");
    }
  }

  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Audio track plan for resampler test failed: " << error
               << L'\n';
    return 1;
  }

  const auto aac_config =
      olouie::audio::MakeAacEncoderConfig(plan.tracks[0], prepared.format,
                                          192000);
  if (!olouie::audio::ValidateAacEncoderConfig(aac_config).Succeeded()) {
    return Fail("Prepared PCM should satisfy the AAC input boundary.");
  }

  const auto s16_format = olouie::audio::MakeSigned16PcmFormat(48000, 2);
  const std::vector<int16_t> s16_samples{-10, 20, 300, -400};
  const auto s16_bytes = BytesFromSigned16(s16_samples);
  result = olouie::audio::PreparePcmForAac(
      s16_format, 2, s16_bytes, 48000, &prepared);
  if (!result.Succeeded() || prepared.data != s16_bytes) {
    return Fail("Prepared S16 PCM should be copied without conversion.");
  }

  result = olouie::audio::PreparePcmForAac(
      float_format, 4, BytesFromFloats(float_samples), 44100, &prepared);
  if (!result.Succeeded() || !prepared.IsValid() ||
      prepared.format.sample_rate != 44100 || prepared.frame_count != 3 ||
      prepared.data.size() != 12) {
    return Fail("Float PCM sample-rate conversion failed.");
  }


  const auto float_44100 = olouie::audio::MakePcmStreamFormat(
      44100, 1, 32, 4, 176400, olouie::audio::PcmSampleEncoding::Float);
  olouie::audio::StreamingPcmResampler streaming(float_44100, 48000);
  const std::vector<float> first_chunk(220, 0.25f);
  const std::vector<float> second_chunk(221, -0.25f);
  olouie::audio::PreparedPcmBuffer first_resampled;
  olouie::audio::PreparedPcmBuffer second_resampled;
  const auto first_result = streaming.Convert(
      220, BytesFromFloats(first_chunk), &first_resampled);
  const auto second_result = streaming.Convert(
      221, BytesFromFloats(second_chunk), &second_resampled);
  if (!first_result.Succeeded() || !second_result.Succeeded() ||
      first_resampled.frame_count != 239 ||
      second_resampled.frame_count != 241 ||
      streaming.total_input_frame_count() != 441 ||
      streaming.total_output_frame_count() != 480) {
    return Fail("Streaming sample-rate conversion accumulated timing drift.");
  }

  olouie::audio::StreamingPcmResampler mono_to_stereo(
      olouie::audio::MakeSigned16PcmFormat(44100, 1), 48000, 2);
  const std::vector<int16_t> mono_samples{1000, -2000, 3000, -4000};
  olouie::audio::PreparedPcmBuffer stereo_resampled;
  const auto stereo_result = mono_to_stereo.Convert(
      4, BytesFromSigned16(mono_samples), &stereo_resampled);
  if (!stereo_result.Succeeded() || !stereo_resampled.IsValid() ||
      stereo_resampled.format.channel_count != 2 ||
      stereo_resampled.frame_count != 4 ||
      ReadSigned16Sample(stereo_resampled.data, 0) != 1000 ||
      ReadSigned16Sample(stereo_resampled.data, 1) != 1000) {
    return Fail("Streaming endpoint conversion should preserve mono audio "
                "when a stereo track is already configured.");
  }

  const auto signed24_format = olouie::audio::MakePcmStreamFormat(
      48000, 2, 24, 6, 288000,
      olouie::audio::PcmSampleEncoding::SignedInteger);
  const std::vector<std::byte> signed24_bytes(6, std::byte{0});
  result = olouie::audio::PreparePcmForAac(
      signed24_format, 1, signed24_bytes, 48000, &prepared);
  if (result.status !=
      olouie::audio::AudioResampleStatus::UnsupportedConversion) {
    return Fail("Unsupported PCM sample formats should be rejected.");
  }

  result = olouie::audio::PreparePcmForAac(
      float_format, 4, std::span<const std::byte>(), 48000, &prepared);
  if (result.status != olouie::audio::AudioResampleStatus::InvalidInput) {
    return Fail("PCM preparation should reject mismatched byte counts.");
  }

  return 0;
}

}  // namespace

int main() {
  ComApartment com;
  if (!com.Initialize()) {
    return Fail("COM initialization failed.");
  }

  if (const int result = VerifyPcmAudioModel(); result != 0) {
    return result;
  }

  if (const int result = VerifyCapturedPcmSinkBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioTrackPlan(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioSourceRouterBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAacEncoderBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAacPacketStoreHandoff(); result != 0) {
    return result;
  }

  if (const int result = VerifyAacEncodeSinkBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyPreparedPcmQueueBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioEncodeWorkerBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyCapturedPcmQueueWriterBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioTrackEncodeChainBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioEncodeSessionBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioEncodeSessionBoundedDrainBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyAudioEncodeSessionBindingBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyAudioSourceSessionDispatcherBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyCapturedPcmSessionSinkBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioCaptureEncodeBridgeBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyAudioCaptureManagerBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioCaptureSmokeOrchestrationBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyAudioCaptureEncodeSetupBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioRecordingSessionBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioCaptureEncodeSmokeBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioLiveCaptureEncodeBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyWasapiCaptureSourceLifecycleBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyMicMonitorBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyAudioResamplerBoundary(); result != 0) {
    return result;
  }

  if (const int result =
          VerifyFlow(olouie::audio::AudioEndpointFlow::Render);
      result != 0) {
    return result;
  }

  if (const int result =
          VerifyFlow(olouie::audio::AudioEndpointFlow::Capture);
      result != 0) {
    return result;
  }

  olouie::audio::LoopbackSmokeResult loopback_result;
  std::wstring error;
  if (olouie::audio::RunDefaultRenderLoopbackSmoke(
          std::chrono::milliseconds(0), &loopback_result, &error)) {
    return Fail("Loopback smoke should reject zero duration.");
  }

  FakeCapturedPcmSink smoke_sink;
  if (olouie::audio::RunDefaultRenderLoopbackSmoke(
          std::chrono::milliseconds(0), &loopback_result, &smoke_sink,
          &error) ||
      smoke_sink.packet_count != 0) {
    return Fail("Loopback smoke sink overload should reject zero duration.");
  }

  olouie::audio::MicCaptureSmokeResult mic_result;
  if (olouie::audio::RunDefaultMicCaptureSmoke(
          std::chrono::milliseconds(0), &mic_result, &error)) {
    return Fail("Microphone smoke should reject zero duration.");
  }

  if (olouie::audio::RunDefaultMicCaptureSmoke(
          std::chrono::milliseconds(0), &mic_result, &smoke_sink, &error) ||
      smoke_sink.packet_count != 0) {
    return Fail("Microphone smoke sink overload should reject zero duration.");
  }

  return 0;
}
