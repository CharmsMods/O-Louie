#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/AudioCaptureEncodeSmoke.h"
#include "audio/AudioCaptureEncodeSetup.h"
#include "audio/AudioCaptureSmoke.h"
#include "audio/AudioEndpointManager.h"
#include "audio/AudioEncodeSession.h"
#include "audio/AudioLiveCaptureEncode.h"
#include "audio/AudioRecordingSession.h"
#include "audio/AudioTrackPlan.h"
#include "audio/CapturedPcmSink.h"
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

class CountingCapturedPcmSink final : public olouie::audio::ICapturedPcmSink {
 public:
  olouie::audio::CapturedPcmSinkResult OnCapturedPcm(
      const olouie::audio::CapturedPcmPacket& packet) override {
    ++packet_count;
    switch (packet.source.kind) {
      case olouie::audio::AudioTrackKind::SystemLoopback:
        ++system_packet_count;
        break;
      case olouie::audio::AudioTrackKind::Microphone:
        ++mic_packet_count;
        break;
      case olouie::audio::AudioTrackKind::ProcessLoopback:
        ++process_packet_count;
        break;
      case olouie::audio::AudioTrackKind::DefaultMixed:
        break;
    }

    olouie::audio::CapturedPcmSinkResult result;
    result.status = olouie::audio::CapturedPcmSinkStatus::Success;
    return result;
  }

  uint32_t packet_count = 0;
  uint32_t system_packet_count = 0;
  uint32_t mic_packet_count = 0;
  uint32_t process_packet_count = 0;
};

constexpr uint32_t kManualAacBitrateBps = 192000;
constexpr size_t kManualEncodeQueueCapacity = 64;

const wchar_t* PcmEncodingName(
    olouie::audio::PcmSampleEncoding encoding) noexcept {
  switch (encoding) {
    case olouie::audio::PcmSampleEncoding::UnsignedInteger:
      return L"unsigned integer";
    case olouie::audio::PcmSampleEncoding::SignedInteger:
      return L"signed integer";
    case olouie::audio::PcmSampleEncoding::Float:
      return L"float";
    case olouie::audio::PcmSampleEncoding::Unknown:
      break;
  }

  return L"unknown";
}

const wchar_t* AudioEncodeSessionStatusName(
    olouie::audio::AudioEncodeSessionStatus status) noexcept {
  switch (status) {
    case olouie::audio::AudioEncodeSessionStatus::Success:
      return L"success";
    case olouie::audio::AudioEncodeSessionStatus::InvalidConfig:
      return L"invalid config";
    case olouie::audio::AudioEncodeSessionStatus::UnknownTrack:
      return L"unknown track";
    case olouie::audio::AudioEncodeSessionStatus::QueueError:
      return L"queue error";
    case olouie::audio::AudioEncodeSessionStatus::DrainError:
      return L"drain error";
    case olouie::audio::AudioEncodeSessionStatus::FlushError:
      return L"flush error";
  }

  return L"unknown";
}

const wchar_t* AudioCaptureEncodeSmokeStatusName(
    olouie::audio::AudioCaptureEncodeSmokeStatus status) noexcept {
  switch (status) {
    case olouie::audio::AudioCaptureEncodeSmokeStatus::Success:
      return L"success";
    case olouie::audio::AudioCaptureEncodeSmokeStatus::InvalidConfig:
      return L"invalid config";
    case olouie::audio::AudioCaptureEncodeSmokeStatus::CaptureFailed:
      return L"capture failed";
    case olouie::audio::AudioCaptureEncodeSmokeStatus::DrainFailed:
      return L"drain failed";
  }

  return L"unknown";
}

const wchar_t* SourceLabel(olouie::audio::AudioTrackKind kind) noexcept {
  switch (kind) {
    case olouie::audio::AudioTrackKind::SystemLoopback:
      return L"default render mix";
    case olouie::audio::AudioTrackKind::Microphone:
      return L"default microphone mix";
    case olouie::audio::AudioTrackKind::ProcessLoopback:
      return L"process loopback";
    case olouie::audio::AudioTrackKind::DefaultMixed:
      return L"default mixed audio";
  }

  return L"unknown audio source";
}

void PrintPcmFormat(const wchar_t* label,
                    const olouie::audio::PcmStreamFormat& format) {
  std::wcout << L"  " << label << L": " << format.sample_rate << L" Hz, "
             << format.channel_count << L" channel(s), "
             << format.bits_per_sample << L" bits/sample, "
             << PcmEncodingName(format.encoding) << L", block align "
             << format.block_align << L'\n';
}

std::filesystem::path MakeManualCaptureEncodeSessionDir() {
  std::error_code error;
  const auto temp_dir = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }

  const auto stamp =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return temp_dir / L"O'LouieAudioCaptureEncodeSmoke" /
         std::to_wstring(stamp);
}

std::filesystem::path MakeManualAudioRecordingSessionDir() {
  std::error_code error;
  const auto temp_dir = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }

  const auto stamp =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return temp_dir / L"O'LouieAudioRecordingSessionSmoke" /
         std::to_wstring(stamp);
}

struct ManualCaptureEncodeSession {
  std::filesystem::path session_dir;
  olouie::record::PacketStore store;
  olouie::audio::AudioCaptureEncodePreflight preflight;
  olouie::audio::AudioCaptureEncodeSessionSetup setup;
};

void PrintCaptureEncodePreflight(
    const olouie::audio::AudioCaptureEncodePreflight& preflight) {
  for (const auto& source : preflight.sources) {
    if (source.included && source.captured_format.IsValid()) {
      PrintPcmFormat(SourceLabel(source.source.kind), source.captured_format);
      continue;
    }

    if (source.requested && !source.required && !source.message.empty()) {
      std::wcout << L"  " << SourceLabel(source.source.kind)
                 << L" encode: deferred for this run (" << source.message
                 << L")\n";
    }
  }
}

void PrintCaptureEncodeSessionSetup(
    const olouie::audio::AudioCaptureEncodeSessionSetup& setup) {
  for (const auto& encoder : setup.encoder_infos) {
    PrintPcmFormat((std::wstring(L"prepared AAC input for track ") +
                    std::to_wstring(encoder.track_id))
                       .c_str(),
                   encoder.prepared_format);
    std::wcout << L"  encoder track " << encoder.track_id << L": "
               << encoder.backend_name << L'\n';
    const auto& output = encoder.output_metadata;
    std::wcout << L"    raw AAC: " << output.sample_rate << L" Hz, "
               << output.channel_count << L" channel(s), "
               << output.bitrate_bps << L" bps, "
               << output.frame_samples << L" samples/frame, ASC "
               << output.audio_specific_config.size() << L" byte(s)\n";
  }
}

bool PrepareManualCaptureEncodeSession(
    size_t queue_capacity,
    ManualCaptureEncodeSession* prepared) {
  if (prepared == nullptr) {
    std::wcerr << L"Manual capture encode setup needs an output destination.\n";
    return false;
  }

  prepared->setup.Reset();
  prepared->preflight.Reset();
  prepared->store.Close();
  prepared->session_dir.clear();

  olouie::audio::AudioCaptureEncodePreflightOptions preflight_options;
  preflight_options.microphone = true;
  preflight_options.require_microphone = false;
  preflight_options.default_mixed_track = false;

  const auto preflight = olouie::audio::BuildAudioCaptureEncodePreflight(
      preflight_options, &prepared->preflight);
  PrintCaptureEncodePreflight(prepared->preflight);
  if (!preflight.Succeeded()) {
    std::wcerr << L"Could not prepare audio capture encode sources: "
               << olouie::audio::AudioCaptureEncodeSetupStatusName(
                      preflight.status);
    if (!preflight.message.empty()) {
      std::wcerr << L" - " << preflight.message;
    }
    std::wcerr << L'\n';
    return false;
  }

  prepared->session_dir = MakeManualCaptureEncodeSessionDir();
  if (prepared->session_dir.empty()) {
    std::wcerr << L"Could not resolve a temporary PacketStore directory.\n";
    return false;
  }

  std::wstring error;
  prepared->store =
      olouie::record::PacketStore::Create(prepared->session_dir,
                                          prepared->preflight.plan.packet_tracks,
                                          &error);
  if (!prepared->store.IsWritable()) {
    std::wcerr << L"Could not create temporary PacketStore: " << error
               << L'\n';
    return false;
  }

  olouie::audio::AudioCaptureEncodeSessionSetupOptions setup_options;
  setup_options.queue_capacity = queue_capacity;
  setup_options.overflow_policy =
      olouie::audio::PreparedPcmOverflowPolicy::RejectNewest;
  setup_options.aac_bitrate_bps = kManualAacBitrateBps;

  const auto setup = olouie::audio::BuildAudioCaptureEncodeSessionSetup(
      prepared->preflight, setup_options, &prepared->store, &prepared->setup);
  if (!setup.Succeeded()) {
    prepared->store.Close();
    std::wcerr << L"Could not set up audio capture encode session: "
               << olouie::audio::AudioCaptureEncodeSetupStatusName(
                      setup.status);
    if (!setup.message.empty()) {
      std::wcerr << L" - " << setup.message;
    }
    std::wcerr << L'\n';
    return false;
  }

  PrintCaptureEncodeSessionSetup(prepared->setup);
  return true;
}

void PrintCaptureEncodeResult(
    const olouie::audio::AudioCaptureEncodeSmokeResult& result) {
  std::wcout << L"  bridge configured: "
             << (result.bridge_configured ? L"yes" : L"no") << L'\n';

  if (result.capture.deferred_mixed_track) {
    std::wcout << L"  default mixed track: deferred\n";
  }

  for (const auto& source : result.capture.sources) {
    std::wcout << L"  source: "
               << olouie::audio::AudioCaptureSourceRuntimeName(source.runtime)
               << L" (track " << source.track_id << L")\n"
               << L"    support: "
               << olouie::audio::AudioCaptureSourceSupportName(source.support)
               << L'\n';
    if (!source.attempted) {
      if (!source.message.empty()) {
        std::wcout << L"    note: " << source.message << L'\n';
      }
      continue;
    }

    std::wcout << L"    result: "
               << (source.succeeded ? L"success" : L"failed") << L'\n';
    if (!source.message.empty()) {
      std::wcout << L"    message: " << source.message << L'\n';
    }
    std::wcout << L"    packets: " << source.capture.packet_count << L'\n'
               << L"    frames: " << source.capture.frame_count << L'\n'
               << L"    silent packets: "
               << source.capture.silent_packet_count << L'\n'
               << L"    discontinuities: "
               << source.capture.data_discontinuity_count << L'\n'
               << L"    timestamp errors: "
               << source.capture.timestamp_error_count << L'\n';
    if (source.capture.format.IsValid()) {
      std::wcout << L"    format: " << source.capture.format.sample_rate
                 << L" Hz, " << source.capture.format.channel_count
                 << L" channel(s), "
                 << source.capture.format.bits_per_sample
                 << L" bits/sample\n";
    }
  }

  std::wcout << L"  capture attempted sources: "
             << result.capture.attempted_source_count << L'\n'
             << L"  capture succeeded sources: "
             << result.capture.succeeded_source_count << L'\n'
             << L"  capture deferred sources: "
             << result.capture.deferred_source_count << L'\n'
             << L"  capture packets: " << result.capture.packet_count << L'\n'
             << L"  capture frames: " << result.capture.frame_count << L'\n'
             << L"  sink received packets: "
             << result.sink_stats.received_packet_count << L'\n'
             << L"  sink queued packets: "
             << result.sink_stats.queued_packet_count << L'\n'
             << L"  sink queue errors: "
             << result.sink_stats.queue_error_count << L'\n'
             << L"  drain status: "
             << AudioEncodeSessionStatusName(result.drain.status) << L'\n';
  if (!result.drain.message.empty()) {
    std::wcout << L"  drain message: " << result.drain.message << L'\n';
  }
}

void PrintLiveCaptureEncodeResult(
    const olouie::audio::AudioLiveCaptureEncodeResult& result) {
  std::wcout << L"  bridge configured: "
             << (result.bridge_configured ? L"yes" : L"no") << L'\n';

  if (result.deferred_mixed_track) {
    std::wcout << L"  default mixed track: deferred\n";
  }

  for (const auto& source : result.sources) {
    std::wcout << L"  source: "
               << olouie::audio::AudioCaptureSourceRuntimeName(source.runtime)
               << L" (track " << source.track_id << L")\n"
               << L"    support: "
               << olouie::audio::AudioCaptureSourceSupportName(source.support)
               << L'\n';
    if (!source.attempted) {
      if (!source.message.empty()) {
        std::wcout << L"    note: " << source.message << L'\n';
      }
      continue;
    }

    std::wcout << L"    start: "
               << olouie::audio::WasapiCaptureSourceStatusName(
                      source.start_result.status)
               << L'\n';
    if (!source.start_result.message.empty()) {
      std::wcout << L"    start message: " << source.start_result.message
                 << L'\n';
    }

    if (source.stopped) {
      std::wcout << L"    final: "
                 << olouie::audio::WasapiCaptureSourceStatusName(
                        source.final_result.status)
                 << L'\n';
      if (!source.final_result.message.empty()) {
        std::wcout << L"    final message: " << source.final_result.message
                   << L'\n';
      }
    }

    std::wcout << L"    packets: " << source.capture.packet_count << L'\n'
               << L"    frames: " << source.capture.frame_count << L'\n'
               << L"    silent packets: "
               << source.capture.silent_packet_count << L'\n'
               << L"    discontinuities: "
               << source.capture.data_discontinuity_count << L'\n'
               << L"    timestamp errors: "
               << source.capture.timestamp_error_count << L'\n'
               << L"    continuity silence packets: "
               << source.synthetic_silence_packet_count << L'\n'
               << L"    retimed packets: "
               << source.retimed_packet_count << L'\n'
               << L"    endpoint invalidations: "
               << source.capture.endpoint_invalidation_count << L'\n'
               << L"    default endpoint changes: "
               << source.capture.default_device_change_count << L'\n'
               << L"    restart attempts / successes: "
               << source.capture.restart_attempt_count << L" / "
               << source.capture.restart_success_count << L'\n'
               << L"    capture format changes: "
               << source.capture.capture_format_change_count << L'\n';
    if (source.capture.format.IsValid()) {
      std::wcout << L"    format: " << source.capture.format.sample_rate
                 << L" Hz, " << source.capture.format.channel_count
                 << L" channel(s), "
                 << source.capture.format.bits_per_sample
                 << L" bits/sample\n";
    }
  }

  std::wcout << L"  attempted sources: " << result.attempted_source_count
             << L'\n'
             << L"  started sources: " << result.started_source_count << L'\n'
             << L"  deferred sources: " << result.deferred_source_count
             << L'\n'
             << L"  capture packets: " << result.packet_count << L'\n'
             << L"  capture frames: " << result.frame_count << L'\n'
             << L"  sink received packets: "
             << result.sink_stats.received_packet_count << L'\n'
             << L"  sink queued packets: "
             << result.sink_stats.queued_packet_count << L'\n'
             << L"  sink queue errors: "
             << result.sink_stats.queue_error_count << L'\n'
             << L"  drain ticks: " << result.drain_tick_count << L'\n'
             << L"  drained PCM blocks: " << result.drained_block_count
             << L'\n'
             << L"  final drain status: "
             << AudioEncodeSessionStatusName(result.final_drain.status)
             << L'\n';
  if (!result.final_drain.message.empty()) {
    std::wcout << L"  final drain message: " << result.final_drain.message
               << L'\n';
  }
  std::wcout << L"  flush status: "
             << AudioEncodeSessionStatusName(result.flush.status) << L'\n';
  if (!result.flush.message.empty()) {
    std::wcout << L"  flush message: " << result.flush.message << L'\n';
  }
}

int PrintFlow(olouie::audio::AudioEndpointFlow flow) {
  std::wstring error;
  std::vector<olouie::audio::AudioEndpointInfo> endpoints;
  if (!olouie::audio::EnumerateActiveAudioEndpoints(flow, &endpoints, &error)) {
    std::wcerr << L"Could not enumerate " << olouie::audio::FlowName(flow)
               << L" endpoints: " << error << L'\n';
    return 1;
  }

  olouie::audio::AudioEndpointInfo default_endpoint;
  const bool has_default =
      olouie::audio::TryGetDefaultAudioEndpoint(flow, &default_endpoint, &error);

  std::wcout << olouie::audio::FlowName(flow) << L" endpoints: "
             << endpoints.size() << L'\n';
  if (has_default) {
    std::wcout << L"  default: " << default_endpoint.name << L'\n';
  } else {
    std::wcout << L"  default: unavailable (" << error << L")\n";
  }

  for (const auto& endpoint : endpoints) {
    std::wcout << L"  - " << endpoint.name;
    if (endpoint.is_default) {
      std::wcout << L" [default]";
    }
    std::wcout << L'\n';
  }

  return 0;
}

void PrintUsage() {
  std::wcout << L"Usage:\n"
             << L"  O'LouieAudioSmoke.exe\n"
             << L"  O'LouieAudioSmoke.exe --loopback [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --mic [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --capture-sources [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --live-sources [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --live-capture-encode [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --capture-encode [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --audio-session [duration_ms]\n"
             << L"  O'LouieAudioSmoke.exe --audio-session-require-mic "
                L"[duration_ms]\n";
}

bool ParseDuration(int argc, wchar_t** argv, std::chrono::milliseconds* value) {
  if (argc < 3) {
    *value = std::chrono::milliseconds(1000);
    return true;
  }

  try {
    const int duration = std::stoi(argv[2]);
    if (duration <= 0 || duration > 30000) {
      return false;
    }

    *value = std::chrono::milliseconds(duration);
    return true;
  } catch (...) {
    return false;
  }
}

int RunLoopbackSmoke(std::chrono::milliseconds duration) {
  olouie::audio::LoopbackSmokeResult result;
  std::wstring error;
  std::wcout << L"Starting system loopback smoke for " << duration.count()
             << L" ms...\n";
  if (!olouie::audio::RunDefaultRenderLoopbackSmoke(duration, &result, &error)) {
    std::wcerr << L"Loopback smoke failed: " << error << L'\n';
    return 1;
  }

  std::wcout << L"  event callback: "
             << (result.used_event_callback ? L"yes" : L"no") << L'\n'
             << L"  packets: " << result.packet_count << L'\n'
             << L"  frames: " << result.frame_count << L'\n'
             << L"  silent packets: " << result.silent_packet_count << L'\n'
             << L"  format: " << result.format.sample_rate << L" Hz, "
             << result.format.channel_count << L" channel(s), "
             << result.format.bits_per_sample << L" bits/sample\n"
             << L"  first device position: "
             << result.first_packet.device_position_frames
             << L'\n'
             << L"  last device position: "
             << result.last_packet.device_position_frames
             << L'\n'
             << L"  first QPC position: "
             << result.first_packet.qpc_position_100ns << L'\n'
             << L"  last QPC position: "
             << result.last_packet.qpc_position_100ns << L'\n';

  return 0;
}

int RunMicSmoke(std::chrono::milliseconds duration) {
  olouie::audio::MicCaptureSmokeResult result;
  std::wstring error;
  std::wcout << L"Starting microphone capture smoke for " << duration.count()
             << L" ms...\n";
  if (!olouie::audio::RunDefaultMicCaptureSmoke(duration, &result, &error)) {
    std::wcerr << L"Microphone smoke failed: " << error << L'\n';
    return 1;
  }

  std::wcout << L"  event callback: "
             << (result.used_event_callback ? L"yes" : L"no") << L'\n'
             << L"  packets: " << result.packet_count << L'\n'
             << L"  frames: " << result.frame_count << L'\n'
             << L"  silent packets: " << result.silent_packet_count << L'\n'
             << L"  format: " << result.format.sample_rate << L" Hz, "
             << result.format.channel_count << L" channel(s), "
             << result.format.bits_per_sample << L" bits/sample\n"
             << L"  first device position: "
             << result.first_packet.device_position_frames
             << L'\n'
             << L"  last device position: "
             << result.last_packet.device_position_frames
             << L'\n'
             << L"  first QPC position: "
             << result.first_packet.qpc_position_100ns << L'\n'
             << L"  last QPC position: "
             << result.last_packet.qpc_position_100ns << L'\n';

  return 0;
}

int RunCaptureSourcesSmoke(std::chrono::milliseconds duration) {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Could not build audio capture source plan: " << error
               << L'\n';
    return 1;
  }

  CountingCapturedPcmSink sink;
  olouie::audio::AudioCaptureSmokeResult result;
  std::wcout << L"Starting manager-driven audio capture smoke for "
             << duration.count() << L" ms per supported source...\n";
  const auto run =
      olouie::audio::RunAudioCaptureSmoke(plan, duration, &sink, &result);

  if (result.deferred_mixed_track) {
    std::wcout << L"  default mixed track: deferred\n";
  }

  for (const auto& source : result.sources) {
    std::wcout << L"  source: "
               << olouie::audio::AudioCaptureSourceRuntimeName(source.runtime)
               << L" (track " << source.track_id << L")\n"
               << L"    support: "
               << olouie::audio::AudioCaptureSourceSupportName(source.support)
               << L'\n';
    if (!source.attempted) {
      if (!source.message.empty()) {
        std::wcout << L"    note: " << source.message << L'\n';
      }
      continue;
    }

    std::wcout << L"    result: "
               << (source.succeeded ? L"success" : L"failed") << L'\n';
    if (!source.message.empty()) {
      std::wcout << L"    message: " << source.message << L'\n';
    }
    std::wcout << L"    packets: " << source.capture.packet_count << L'\n'
               << L"    frames: " << source.capture.frame_count << L'\n'
               << L"    silent packets: "
               << source.capture.silent_packet_count << L'\n';
    if (source.capture.format.IsValid()) {
      std::wcout << L"    format: " << source.capture.format.sample_rate
                 << L" Hz, " << source.capture.format.channel_count
                 << L" channel(s), "
                 << source.capture.format.bits_per_sample
                 << L" bits/sample\n";
    }
  }

  std::wcout << L"  attempted sources: " << result.attempted_source_count
             << L'\n'
             << L"  succeeded sources: " << result.succeeded_source_count
             << L'\n'
             << L"  deferred sources: " << result.deferred_source_count
             << L'\n'
             << L"  total packets: " << result.packet_count << L'\n'
             << L"  total frames: " << result.frame_count << L'\n'
             << L"  sink packets: " << sink.packet_count << L" (system "
             << sink.system_packet_count << L", mic " << sink.mic_packet_count
             << L", process " << sink.process_packet_count << L")\n";

  if (!run.Succeeded()) {
    std::wcerr << L"Manager-driven audio capture smoke failed: "
               << run.message << L'\n';
    return 1;
  }

  return 0;
}

int RunLiveSourcesSmoke(std::chrono::milliseconds duration) {
  olouie::audio::AudioTrackPlan plan;
  olouie::audio::AudioTrackPlanOptions options;
  options.microphone = true;
  std::wstring error;
  if (!olouie::audio::BuildAudioTrackPlan(options, &plan, &error)) {
    std::wcerr << L"Could not build live audio source plan: " << error
               << L'\n';
    return 1;
  }

  CountingCapturedPcmSink sink;
  olouie::audio::AudioCaptureManager manager;
  const auto configured = manager.Configure(plan, &sink);
  if (!configured.Succeeded()) {
    std::wcerr << L"Could not configure live audio sources: "
               << configured.message << L'\n';
    return 1;
  }

  struct RunningSource {
    olouie::audio::AudioCaptureSourceBinding binding;
    std::unique_ptr<olouie::audio::WasapiCaptureSource> source;
    olouie::audio::WasapiCaptureSourceResult start_result;
  };

  std::vector<RunningSource> running_sources;
  bool start_failed = false;

  std::wcout << L"Starting live audio sources for " << duration.count()
             << L" ms...\n";
  if (manager.has_deferred_mixed_track()) {
    std::wcout << L"  default mixed track: deferred\n";
  }

  for (const auto& binding : manager.sources()) {
    std::wcout << L"  source: "
               << olouie::audio::AudioCaptureSourceRuntimeName(binding.runtime)
               << L" (track " << binding.track_id << L")\n"
               << L"    support: "
               << olouie::audio::AudioCaptureSourceSupportName(binding.support)
               << L'\n';

    if (binding.support == olouie::audio::AudioCaptureSourceSupport::Deferred) {
      std::wcout << L"    start: deferred\n";
      continue;
    }

    auto source =
        std::make_unique<olouie::audio::WasapiCaptureSource>(binding.source);
    auto started = source->Start(binding.sink);
    std::wcout << L"    start: "
               << olouie::audio::WasapiCaptureSourceStatusName(started.status)
               << L'\n';
    if (!started.message.empty()) {
      std::wcout << L"    message: " << started.message << L'\n';
    }

    if (!started.Succeeded()) {
      start_failed = true;
      continue;
    }

    running_sources.push_back(
        RunningSource{binding, std::move(source), std::move(started)});
  }

  if (start_failed) {
    for (auto& running : running_sources) {
      running.source->Stop();
    }
    std::wcerr << L"At least one live WASAPI source failed to start.\n";
    return 1;
  }

  std::this_thread::sleep_for(duration);

  int result = 0;
  for (auto& running : running_sources) {
    running.source->Stop();
    const auto last = running.source->LastResult();
    const auto stats = running.source->SnapshotStats();
    std::wcout << L"  stopped: "
               << olouie::audio::AudioCaptureSourceRuntimeName(
                      running.binding.runtime)
               << L" (track " << running.binding.track_id << L")\n"
               << L"    final status: "
               << olouie::audio::WasapiCaptureSourceStatusName(last.status)
               << L'\n';
    if (!last.message.empty()) {
      std::wcout << L"    message: " << last.message << L'\n';
    }
    std::wcout << L"    event callback: "
               << (stats.used_event_callback ? L"yes" : L"no") << L'\n'
               << L"    packets: " << stats.packet_count << L'\n'
               << L"    frames: " << stats.frame_count << L'\n'
               << L"    silent packets: " << stats.silent_packet_count
               << L'\n';
    if (stats.format.IsValid()) {
      std::wcout << L"    format: " << stats.format.sample_rate << L" Hz, "
                 << stats.format.channel_count << L" channel(s), "
                 << stats.format.bits_per_sample << L" bits/sample\n";
    }
    if (!last.Succeeded()) {
      result = 1;
    }
  }

  std::wcout << L"  sink packets: " << sink.packet_count << L" (system "
             << sink.system_packet_count << L", mic " << sink.mic_packet_count
             << L", process " << sink.process_packet_count << L")\n";
  return result;
}

int RunCaptureEncodeSmoke(std::chrono::milliseconds duration) {
  std::wcout << L"Preparing real AAC capture encode smoke...\n";
  ManualCaptureEncodeSession prepared;
  if (!PrepareManualCaptureEncodeSession(kManualEncodeQueueCapacity,
                                         &prepared)) {
    return 1;
  }

  olouie::audio::AudioCaptureEncodeSmokeResult result;
  std::wcout << L"Starting real AAC capture encode smoke for "
             << duration.count() << L" ms per planned source...\n";
  const auto run = olouie::audio::RunAudioCaptureEncodeSmoke(
      prepared.preflight.plan, duration, prepared.setup.session.get(),
      &result);
  const auto flush = prepared.setup.session->FlushAllTracks();
  const auto index = prepared.store.SnapshotIndex();
  prepared.store.Close();

  PrintCaptureEncodeResult(result);
  std::wcout << L"  flush status: "
             << AudioEncodeSessionStatusName(flush.status) << L'\n';
  if (!flush.message.empty()) {
    std::wcout << L"  flush message: " << flush.message << L'\n';
  }
  std::wcout << L"  queued PCM packets: "
             << prepared.setup.session->stats().queued_packet_count << L'\n'
             << L"  drained PCM blocks: "
             << prepared.setup.session->stats().drained_block_count << L'\n'
             << L"  flushed tracks: "
             << prepared.setup.session->stats().flushed_track_count << L'\n'
             << L"  PacketStore packets: " << index.size() << L'\n'
             << L"  PacketStore path: " << prepared.session_dir.native()
             << L'\n';

  if (!run.Succeeded()) {
    std::wcerr << L"Real AAC capture encode smoke failed: "
               << AudioCaptureEncodeSmokeStatusName(run.status);
    if (!run.message.empty()) {
      std::wcerr << L" - " << run.message;
    }
    std::wcerr << L'\n';
    return 1;
  }

  if (!flush.Succeeded()) {
    std::wcerr << L"Real AAC capture encode smoke flush failed: "
               << flush.message << L'\n';
    return 1;
  }

  return 0;
}

int RunLiveCaptureEncodeSmoke(std::chrono::milliseconds duration) {
  std::wcout << L"Preparing real AAC live capture encode smoke...\n";
  ManualCaptureEncodeSession prepared;
  if (!PrepareManualCaptureEncodeSession(kManualEncodeQueueCapacity,
                                         &prepared)) {
    return 1;
  }

  olouie::audio::AudioLiveCaptureEncodeOptions options;
  options.duration = duration;
  options.drain_interval = std::chrono::milliseconds(10);
  options.max_blocks_per_drain_tick = 16;
  olouie::record::SessionClock clock;
  std::wstring clock_error;
  if (!olouie::record::CaptureSessionClock(&clock, &clock_error)) {
    std::wcerr << L"Could not establish live audio smoke clock: "
               << clock_error << L'\n';
    return 1;
  }
  options.qpc_origin_ns = clock.origin_ns;
  options.maintain_track_continuity = true;

  olouie::audio::AudioLiveCaptureEncodeResult result;
  std::wcout << L"Starting real AAC live capture encode smoke for "
             << duration.count() << L" ms...\n";
  const auto run = olouie::audio::RunAudioLiveCaptureEncode(
      prepared.preflight.plan, options, prepared.setup.session.get(),
      &result);
  const auto index = prepared.store.SnapshotIndex();
  prepared.store.Close();

  PrintLiveCaptureEncodeResult(result);
  std::wcout << L"  queued PCM packets: "
             << prepared.setup.session->stats().queued_packet_count << L'\n'
             << L"  queue failures: "
             << prepared.setup.session->stats().queue_failure_count << L'\n'
             << L"  session drained PCM blocks: "
             << prepared.setup.session->stats().drained_block_count << L'\n'
             << L"  flushed tracks: "
             << prepared.setup.session->stats().flushed_track_count << L'\n'
             << L"  PacketStore packets: " << index.size() << L'\n'
             << L"  PacketStore path: " << prepared.session_dir.native()
             << L'\n';

  if (!run.Succeeded()) {
    std::wcerr << L"Real AAC live capture encode smoke failed: "
               << olouie::audio::AudioLiveCaptureEncodeStatusName(run.status);
    if (!run.message.empty()) {
      std::wcerr << L" - " << run.message;
    }
    std::wcerr << L'\n';
    return 1;
  }

  return 0;
}

int RunAudioRecordingSessionSmoke(std::chrono::milliseconds duration,
                                  bool require_microphone) {
  std::wcout << L"Preparing audio recording session smoke...\n";

  olouie::audio::AudioRecordingSessionOptions options;
  options.preflight.microphone = true;
  options.preflight.require_microphone = require_microphone;
  options.preflight.default_mixed_track = false;
  options.setup.queue_capacity = kManualEncodeQueueCapacity;
  options.setup.overflow_policy =
      olouie::audio::PreparedPcmOverflowPolicy::RejectNewest;
  options.setup.aac_bitrate_bps = kManualAacBitrateBps;
  options.live.duration = duration;
  options.live.drain_interval = std::chrono::milliseconds(10);
  options.live.max_blocks_per_drain_tick = 16;
  olouie::record::SessionClock clock;
  std::wstring clock_error;
  if (!olouie::record::CaptureSessionClock(&clock, &clock_error)) {
    std::wcerr << L"Could not establish audio session smoke clock: "
               << clock_error << L'\n';
    return 1;
  }
  options.live.qpc_origin_ns = clock.origin_ns;
  options.live.maintain_track_continuity = true;

  olouie::audio::AudioRecordingSession session(options);
  auto result = session.Preflight();
  PrintCaptureEncodePreflight(session.preflight());
  if (!result.Succeeded()) {
    std::wcerr << L"Audio recording session preflight failed: "
               << olouie::audio::AudioRecordingSessionStatusName(
                      result.status);
    if (!result.message.empty()) {
      std::wcerr << L" - " << result.message;
    }
    std::wcerr << L'\n';
    return 1;
  }

  const auto session_dir = MakeManualAudioRecordingSessionDir();
  if (session_dir.empty()) {
    std::wcerr << L"Could not resolve a temporary PacketStore directory.\n";
    return 1;
  }

  std::wstring error;
  auto store =
      olouie::record::PacketStore::Create(session_dir,
                                          session.preflight().plan.packet_tracks,
                                          &error);
  if (!store.IsWritable()) {
    std::wcerr << L"Could not create temporary PacketStore: " << error
               << L'\n';
    return 1;
  }

  result = session.Prepare(&store);
  if (!result.Succeeded()) {
    store.Close();
    std::wcerr << L"Audio recording session prepare failed: "
               << olouie::audio::AudioRecordingSessionStatusName(
                      result.status);
    if (!result.message.empty()) {
      std::wcerr << L" - " << result.message;
    }
    std::wcerr << L'\n';
    return 1;
  }

  PrintCaptureEncodeSessionSetup(session.setup());

  std::wcout << L"Starting audio recording session smoke for "
             << duration.count() << L" ms...\n";
  result = session.RunForDuration(duration);
  const auto index = store.SnapshotIndex();
  store.Close();

  PrintLiveCaptureEncodeResult(session.last_live_result());
  std::wcout << L"  queued PCM packets: "
             << session.encode_session()->stats().queued_packet_count << L'\n'
             << L"  queue failures: "
             << session.encode_session()->stats().queue_failure_count << L'\n'
             << L"  session drained PCM blocks: "
             << session.encode_session()->stats().drained_block_count << L'\n'
             << L"  flushed tracks: "
             << session.encode_session()->stats().flushed_track_count << L'\n'
             << L"  PacketStore packets: " << index.size() << L'\n'
             << L"  PacketStore path: " << session_dir.native() << L'\n';

  if (!result.Succeeded()) {
    std::wcerr << L"Audio recording session smoke failed: "
               << olouie::audio::AudioRecordingSessionStatusName(
                      result.status);
    if (!result.message.empty()) {
      std::wcerr << L" - " << result.message;
    }
    std::wcerr << L'\n';
    return 1;
  }

  if (require_microphone) {
    const auto& live_result = session.last_live_result();
    const auto microphone = std::find_if(
        live_result.sources.begin(), live_result.sources.end(),
        [](const olouie::audio::AudioLiveCaptureEncodeSourceResult& source) {
          return source.runtime ==
                 olouie::audio::AudioCaptureSourceRuntime::Microphone;
        });
    if (microphone == live_result.sources.end() || !microphone->started ||
        microphone->capture.packet_count == 0) {
      std::wcerr << L"Required microphone smoke did not receive a hardware "
                    L"capture packet.\n";
      return 1;
    }
  }

  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool run_loopback = argc >= 2 && std::wstring(argv[1]) == L"--loopback";
  const bool run_mic = argc >= 2 && std::wstring(argv[1]) == L"--mic";
  const bool run_capture_sources =
      argc >= 2 && std::wstring(argv[1]) == L"--capture-sources";
  const bool run_live_sources =
      argc >= 2 && std::wstring(argv[1]) == L"--live-sources";
  const bool run_live_capture_encode =
      argc >= 2 && std::wstring(argv[1]) == L"--live-capture-encode";
  const bool run_capture_encode =
      argc >= 2 && std::wstring(argv[1]) == L"--capture-encode";
  const bool run_audio_session =
      argc >= 2 && std::wstring(argv[1]) == L"--audio-session";
  const bool run_audio_session_require_mic =
      argc >= 2 &&
      std::wstring(argv[1]) == L"--audio-session-require-mic";
  if (argc >= 2 && !run_loopback && !run_mic && !run_capture_sources &&
      !run_live_sources && !run_live_capture_encode && !run_capture_encode &&
      !run_audio_session && !run_audio_session_require_mic) {
    PrintUsage();
    return 2;
  }

  std::chrono::milliseconds smoke_duration(0);
  if ((run_loopback || run_mic || run_capture_sources || run_live_sources ||
       run_live_capture_encode || run_capture_encode || run_audio_session ||
       run_audio_session_require_mic) &&
      !ParseDuration(argc, argv, &smoke_duration)) {
    PrintUsage();
    return 2;
  }

  ComApartment com;
  if (!com.Initialize()) {
    std::wcerr << L"COM initialization failed.\n";
    return 1;
  }

  int result = 0;
  result |= PrintFlow(olouie::audio::AudioEndpointFlow::Render);
  result |= PrintFlow(olouie::audio::AudioEndpointFlow::Capture);
  if (run_loopback) {
    result |= RunLoopbackSmoke(smoke_duration);
  }
  if (run_mic) {
    result |= RunMicSmoke(smoke_duration);
  }
  if (run_capture_sources) {
    result |= RunCaptureSourcesSmoke(smoke_duration);
  }
  if (run_live_sources) {
    result |= RunLiveSourcesSmoke(smoke_duration);
  }
  if (run_live_capture_encode) {
    result |= RunLiveCaptureEncodeSmoke(smoke_duration);
  }
  if (run_capture_encode) {
    result |= RunCaptureEncodeSmoke(smoke_duration);
  }
  if (run_audio_session) {
    result |= RunAudioRecordingSessionSmoke(smoke_duration, false);
  }
  if (run_audio_session_require_mic) {
    result |= RunAudioRecordingSessionSmoke(smoke_duration, true);
  }
  return result;
}
