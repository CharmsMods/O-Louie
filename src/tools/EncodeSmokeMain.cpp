#include <objbase.h>

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <winrt/base.h>

#include "capture/WgcMonitorCapture.h"
#include "encode/H264PacketStore.h"
#include "encode/MfHardwareH264EncoderProbe.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoLiveCaptureEncode.h"
#include "encode/VideoRecordingBootstrap.h"
#include "encode/VideoRecordingMetadata.h"
#include "encode/VideoRecordingRunSession.h"
#include "encode/VideoRecordingSession.h"
#include "graphics/D3D11DeviceContext.h"
#include "graphics/DisplayManager.h"
#include "record/Mp4Muxer.h"
#include "record/PacketStore.h"
#include "record/VideoExportPlan.h"

namespace {

constexpr uint32_t kSyntheticSubmitFrameCount = 3;
constexpr uint32_t kWgcSubmitFrameCount = 3;
constexpr int64_t kWgcTimestampTicksPerSecond = 10000000;

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

void PrintUsage() {
  std::wcout
      << L"Usage:\n"
      << L"  O'LouieEncodeSmoke.exe\n"
      << L"  O'LouieEncodeSmoke.exe --h264-probe "
         L"[width height fps bitrate_mbps] [--capture-first]\n"
      << L"  O'LouieEncodeSmoke.exe --h264-session "
         L"[width height fps bitrate_mbps] [--capture-first]\n"
      << L"  O'LouieEncodeSmoke.exe --h264-submit "
         L"[width height fps bitrate_mbps] [--capture-first]\n"
      << L"  O'LouieEncodeSmoke.exe --h264-bgra-submit "
         L"[width height fps bitrate_mbps] [--capture-first]\n"
      << L"  O'LouieEncodeSmoke.exe --h264-wgc-submit "
         L"[duration_ms width height fps bitrate_mbps] [--capture-first]\n";
}

bool ParseUInt32(const wchar_t* text, uint32_t* value) {
  if (text == nullptr || value == nullptr) {
    return false;
  }

  try {
    const unsigned long parsed = std::stoul(text);
    if (parsed > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseConfig(int argc,
                 wchar_t** argv,
                 olouie::encode::MfHardwareH264EncoderConfig* config) {
  if (config == nullptr) {
    return false;
  }

  *config = {};
  if (argc == 1 || argc == 2) {
    return true;
  }

  if (argc != 6) {
    return false;
  }

  uint32_t bitrate_mbps = 0;
  return ParseUInt32(argv[2], &config->width) &&
         ParseUInt32(argv[3], &config->height) &&
         ParseUInt32(argv[4], &config->fps_numerator) &&
         ParseUInt32(argv[5], &bitrate_mbps) && bitrate_mbps > 0 &&
         bitrate_mbps <= 4000 &&
         bitrate_mbps <=
             std::numeric_limits<uint32_t>::max() / 1000000u &&
         (config->bitrate_bps = bitrate_mbps * 1000000u) > 0;
}

bool ParseWgcSubmitConfig(
    int argc,
    wchar_t** argv,
    std::chrono::milliseconds* duration,
    olouie::encode::MfHardwareH264EncoderConfig* config) {
  if (duration == nullptr || config == nullptr) {
    return false;
  }

  *duration = std::chrono::milliseconds(2000);
  *config = {};
  if (argc == 2) {
    return true;
  }

  uint32_t duration_ms = 0;
  if (argc == 3) {
    if (!ParseUInt32(argv[2], &duration_ms) || duration_ms == 0) {
      return false;
    }
    *duration = std::chrono::milliseconds(duration_ms);
    return true;
  }

  if (argc != 7 || !ParseUInt32(argv[2], &duration_ms) ||
      duration_ms == 0) {
    return false;
  }

  uint32_t bitrate_mbps = 0;
  const bool parsed_config =
      ParseUInt32(argv[3], &config->width) &&
      ParseUInt32(argv[4], &config->height) &&
      ParseUInt32(argv[5], &config->fps_numerator) &&
      ParseUInt32(argv[6], &bitrate_mbps) && bitrate_mbps > 0 &&
      bitrate_mbps <= 4000 &&
      bitrate_mbps <=
          std::numeric_limits<uint32_t>::max() / 1000000u &&
      (config->bitrate_bps = bitrate_mbps * 1000000u) > 0;
  if (!parsed_config) {
    return false;
  }

  *duration = std::chrono::milliseconds(duration_ms);
  return true;
}

int RunH264Probe(const olouie::encode::MfHardwareH264EncoderConfig& config) {
  olouie::encode::MfHardwareH264EncoderProbeOptions options;
  options.include_local_mfts = true;

  std::wcout << L"Probing Media Foundation hardware H.264 encoders...\n"
             << L"  requested: " << config.width << L"x" << config.height
             << L" @ " << config.fps_numerator << L"/"
             << config.fps_denominator << L" fps, "
             << (config.bitrate_bps / 1000000u) << L" Mbps, GOP "
             << config.gop_seconds << L"s, B-frames "
             << config.max_b_frames << L", performance "
             << olouie::performance::CapturePerformanceModeName(
                    config.performance_mode)
             << L'\n';

  const auto result =
      olouie::encode::ProbeMfHardwareH264Encoder(config, options);
  std::wcout << L"  status: "
             << olouie::encode::MfHardwareH264EncoderProbeStatusName(
                    result.status)
             << L'\n';
  if (!result.message.empty()) {
    std::wcout << L"  message: " << result.message << L'\n';
  }

  std::wcout << L"  hardware encoder count: " << result.encoders.size()
             << L'\n';
  for (size_t index = 0; index < result.encoders.size(); ++index) {
    const auto& encoder = result.encoders[index];
    std::wcout << L"  - " << encoder.name;
    if (index == result.selected_encoder_index) {
      std::wcout << L" [selected]";
    }
    std::wcout << L'\n';
    if (!encoder.clsid.empty()) {
      std::wcout << L"    clsid: " << encoder.clsid << L'\n';
    }
    std::wcout << L"    enum flags: 0x" << std::hex
               << encoder.enumeration_flags << std::dec << L'\n';
  }

  return result.Succeeded() ? 0 : 1;
}

void PrintCodecSetting(
    const olouie::encode::MfHardwareH264CodecSettingResult& setting) {
  std::wcout << L"    " << setting.name << L": supported="
             << (setting.supported ? L"yes" : L"no")
             << L", modifiable="
             << (setting.modifiable ? L"yes" : L"no") << L", ";
  if (!setting.attempted) {
    std::wcout << L"not attempted";
  } else if (setting.applied) {
    std::wcout << L"applied";
  } else {
    std::wcout << L"rejected";
  }
  std::wcout << L" (requested " << setting.requested_value;
  if (setting.read_back) {
    std::wcout << L", accepted " << setting.accepted_value;
  }
  std::wcout << L")\n";
  if (!setting.message.empty()) {
    std::wcout << L"      " << setting.message << L'\n';
  }
}

int64_t FrameDurationNs(
    const olouie::encode::MfHardwareH264EncoderConfig& config) {
  return static_cast<int64_t>(
      (1000000000ull * config.fps_denominator) / config.fps_numerator);
}

std::filesystem::path MakeManualH264PacketStoreSessionDir() {
  return std::filesystem::temp_directory_path() /
         L"O'LouieEncodeSmokeH264PacketStore" / L"session";
}

HRESULT CreateGeneratedBgraTexture(ID3D11Device* device,
                                   uint32_t width,
                                   uint32_t height,
                                   UINT bind_flags,
                                   uint32_t frame_index,
                                   ID3D11Texture2D** texture) {
  if (device == nullptr || width == 0 || height == 0 || texture == nullptr) {
    return E_INVALIDARG;
  }

  std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
      bgra[offset + 0u] = static_cast<uint8_t>((x + frame_index * 17u) % 256u);
      bgra[offset + 1u] = static_cast<uint8_t>((y + frame_index * 29u) % 256u);
      bgra[offset + 2u] =
          static_cast<uint8_t>((x + y + frame_index * 11u) % 256u);
      bgra[offset + 3u] = 255u;
    }
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bind_flags;

  D3D11_SUBRESOURCE_DATA source_data{};
  source_data.pSysMem = bgra.data();
  source_data.SysMemPitch = width * 4u;

  return device->CreateTexture2D(&desc, &source_data, texture);
}

int RunH264WgcSession(
    const olouie::encode::MfHardwareH264EncoderConfig& config,
    std::chrono::milliseconds duration) {
  olouie::encode::MfHardwareH264EncoderProbeOptions options;
  options.include_local_mfts = true;

  std::wcout << L"Submitting copied WGC monitor frames through GPU conversion "
                L"to Media Foundation hardware H.264 session...\n"
             << L"  capture duration: " << duration.count() << L" ms\n"
             << L"  requested encode: " << config.width << L"x"
             << config.height << L" @ " << config.fps_numerator << L"/"
             << config.fps_denominator << L" fps, "
             << (config.bitrate_bps / 1000000u) << L" Mbps, GOP "
             << config.gop_seconds << L"s, B-frames "
             << config.max_b_frames << L", performance "
             << olouie::performance::CapturePerformanceModeName(
                    config.performance_mode)
             << L'\n';

  constexpr uint32_t kVideoTrackId = 1;
  const auto session_dir = MakeManualH264PacketStoreSessionDir();
  std::filesystem::remove_all(session_dir.parent_path());

  olouie::encode::VideoRecordingBootstrapOptions bootstrap_options;
  bootstrap_options.packet_store_session_dir = session_dir;
  bootstrap_options.encoder_probe_options = options;
  bootstrap_options.preflight.video_track_id = kVideoTrackId;
  bootstrap_options.preflight.encoder_config = config;
  bootstrap_options.preflight.queue_capacity = kWgcSubmitFrameCount;
  bootstrap_options.preflight.drain_frame_budget = kWgcSubmitFrameCount;
  bootstrap_options.preflight.session_drain_timeout_ms = 3000;
  bootstrap_options.preflight.live = olouie::encode::VideoLiveCaptureEncodeOptions{
      duration, std::chrono::milliseconds(10), kWgcSubmitFrameCount,
      kWgcSubmitFrameCount, kWgcTimestampTicksPerSecond, true};

  olouie::encode::VideoRecordingBootstrapSession bootstrap;
  const auto bootstrap_result =
      olouie::encode::BuildVideoRecordingBootstrapSession(bootstrap_options,
                                                          &bootstrap);
  std::wcout << L"  WGC video bootstrap status: "
             << olouie::encode::VideoRecordingBootstrapStatusName(
                    bootstrap_result.status)
             << L'\n';
  if (!bootstrap_result.message.empty()) {
    std::wcout << L"  WGC video bootstrap message: "
               << bootstrap_result.message << L'\n';
  }
  if (!bootstrap_result.Succeeded() || !bootstrap.IsPrepared()) {
    return 1;
  }

  std::wcout << L"  D3D11 adapter: "
             << bootstrap.d3d.adapter_description() << L'\n';

  const auto& session_result = bootstrap_result.encoder_result;
  std::wcout << L"  encoder session status: "
             << olouie::encode::MfHardwareH264EncoderSessionStatusName(
                    session_result.status)
             << L'\n';
  if (!session_result.message.empty()) {
    std::wcout << L"  encoder session message: " << session_result.message
               << L'\n';
  }

  const auto& info = session_result.info;
  if (!info.encoder.name.empty()) {
    std::wcout << L"  encoder: " << info.encoder.name << L'\n';
  }
  std::wcout << L"  media type: " << info.media_type.width << L"x"
             << info.media_type.height << L" @ "
             << info.media_type.fps_numerator << L"/"
             << info.media_type.fps_denominator << L" fps\n"
             << L"  D3D11 aware: "
             << (info.d3d11_aware ? L"yes" : L"no") << L'\n'
             << L"  DXGI manager attached: "
             << (info.device_manager_attached ? L"yes" : L"no") << L'\n';

  const auto& preflight_result = bootstrap_result.preflight_result;
  std::wcout << L"  WGC video recording preflight status: "
             << olouie::encode::VideoRecordingRunSessionStatusName(
                    preflight_result.status)
             << L'\n';
  if (!preflight_result.message.empty()) {
    std::wcout << L"  WGC video recording preflight message: "
               << preflight_result.message << L'\n';
  }
  const auto& setup_result = bootstrap_result.prepare_result;
  std::wcout << L"  WGC video recording setup status: "
             << olouie::encode::VideoRecordingRunSessionStatusName(
                    setup_result.status)
             << L'\n';
  if (!setup_result.message.empty()) {
    std::wcout << L"  WGC video recording setup message: "
               << setup_result.message << L'\n';
  }
  auto* video_recording = bootstrap.recording_session.get();
  auto* video_chain = video_recording->encode_chain();
  const auto& convert_plan = video_chain->conversion_plan();
  std::wcout << L"  conversion: " << convert_plan.source_width << L"x"
             << convert_plan.source_height << L" BGRA -> "
             << convert_plan.output_width << L"x"
             << convert_plan.output_height << L" NV12\n";

  std::wcout << L"  starting WGC frame-copy smoke...\n";
  const auto video_run = video_recording->Run();
  const auto& live_result = video_recording->last_live_result();
  const auto& live = video_run.live_result;
  const auto& capture_result = live_result.capture;
  std::wcout << L"  WGC video recording run status: "
             << olouie::encode::VideoRecordingRunSessionStatusName(
                    video_run.status)
             << L'\n';
  if (!video_run.message.empty()) {
    std::wcout << L"  WGC video recording run message: "
               << video_run.message << L'\n';
  }
  std::wcout << L"  WGC live video status: "
             << olouie::encode::VideoLiveCaptureEncodeStatusName(live.status)
             << L'\n';
  if (!live.message.empty()) {
    std::wcout << L"  WGC live video message: " << live.message << L'\n';
  }
  std::wcout << L"  WGC supported: "
             << (capture_result.supported ? L"yes" : L"no") << L'\n'
             << L"  WGC frames observed: " << capture_result.frame_count
             << L'\n'
             << L"  WGC frames copied: "
             << capture_result.copied_frame_count << L'\n'
             << L"  WGC frames dropped after cap: "
             << capture_result.dropped_frame_count << L'\n'
             << L"  WGC bridge first timestamp ticks: "
             << live_result.first_timestamp_ticks << L'\n'
             << L"  WGC bridge queued frames: "
             << live_result.bridge_stats.queued_frame_count << L'\n'
             << L"  WGC bridge dropped frames: "
             << live_result.bridge_stats.dropped_frame_count << L'\n'
             << L"  WGC live drain ticks: "
             << live_result.drain_tick_count << L'\n'
             << L"  WGC live drained frames: "
             << live_result.drained_frame_count << L'\n';
  if (!live.Succeeded()) {
    return 1;
  }
  if (capture_result.copied_frame_count < kWgcSubmitFrameCount) {
    std::wcerr << L"  expected " << kWgcSubmitFrameCount
               << L" copied WGC frames for encode smoke.\n";
    return 1;
  }
  const auto& converter_stats = video_chain->converter_stats();
  std::wcout << L"  converter input views created/reused: "
             << converter_stats.input_view_create_count << L"/"
             << converter_stats.input_view_reuse_count << L'\n'
             << L"  converter output views created/reused: "
             << converter_stats.output_view_create_count << L"/"
             << converter_stats.output_view_reuse_count << L'\n';
  if (converter_stats.output_view_create_count != 1 ||
      (capture_result.copied_frame_count > 1 &&
       converter_stats.output_view_reuse_count == 0)) {
    std::wcerr << L"  VideoProcessor output view was not reused.\n";
    return 1;
  }

  const auto worker_result = live_result.final_drain;
  std::wcout << L"  WGC final drain status: "
             << olouie::encode::VideoEncodeWorkerStatusName(
                    worker_result.status)
             << L'\n';
  if (!worker_result.message.empty()) {
    std::wcout << L"  WGC final drain message: "
               << worker_result.message << L'\n';
  }
  std::wcout << L"  WGC final drain popped frames: "
             << worker_result.popped_frame_count << L'\n'
             << L"  WGC final drain processed frames: "
             << worker_result.processed_frame_count << L'\n'
             << L"  WGC final drain failed frames: "
             << worker_result.failed_frame_count << L'\n'
             << L"  WGC final drain remaining frames: "
             << worker_result.remaining_frame_count << L'\n'
             << L"  WGC final drain dropped frames: "
             << worker_result.dropped_frame_count << L'\n';
  if (!worker_result.Succeeded()) {
    if (!worker_result.first_failure.message.empty()) {
      std::wcerr << L"  First BGRA session failure: "
                 << olouie::encode::VideoRecordingSessionStatusName(
                        worker_result.first_failure.status)
                 << L" - " << worker_result.first_failure.message << L'\n';
    }
    return 1;
  }

  const auto packet_store_index = bootstrap.packet_store.SnapshotIndex();
  size_t total_bytes = 0;
  for (const auto& packet : packet_store_index) {
    total_bytes += packet.payload_size;
  }

  if (packet_store_index.size() !=
      video_chain->session_stats().appended_packet_count) {
    std::wcerr << L"  PacketStore packet count did not match session output.\n";
    return 1;
  }

  olouie::encode::VideoRecordingMetadata video_metadata;
  const auto metadata_result =
      olouie::encode::BuildVideoRecordingMetadata(bootstrap, &video_metadata);
  std::wcout << L"  video metadata status: "
             << olouie::encode::VideoRecordingMetadataStatusName(
                    metadata_result.status)
             << L'\n';
  if (!metadata_result.message.empty()) {
    std::wcout << L"  video metadata message: " << metadata_result.message
               << L'\n';
  }
  if (!metadata_result.Succeeded() || !video_metadata.IsReady() ||
      video_metadata.video_track.track_id != kVideoTrackId ||
      video_metadata.h264.avcc_extradata.empty() ||
      video_metadata.source_width != convert_plan.source_width ||
      video_metadata.output_width != convert_plan.output_width ||
      video_metadata.packet_store_session_dir !=
          bootstrap.packet_store.session_dir()) {
    std::wcerr << L"  Video recording metadata was not ready.\n";
    return 1;
  }

  const auto manifest =
      olouie::encode::BuildVideoRecordingSessionManifest(video_metadata);
  const auto manifest_write =
      olouie::record::WriteSessionManifest(manifest);
  std::wcout << L"  session manifest write status: "
             << olouie::record::SessionManifestStatusName(
                    manifest_write.status)
             << L'\n';
  if (!manifest_write.message.empty()) {
    std::wcout << L"  session manifest write message: "
               << manifest_write.message << L'\n';
  }
  if (!manifest_write.Succeeded()) {
    return 1;
  }

  olouie::record::SessionManifest recovered_manifest;
  const auto manifest_read = olouie::record::ReadSessionManifest(
      video_metadata.packet_store_session_dir, &recovered_manifest);
  std::wcout << L"  session manifest read status: "
             << olouie::record::SessionManifestStatusName(
                    manifest_read.status)
             << L'\n';
  if (!manifest_read.message.empty()) {
    std::wcout << L"  session manifest read message: "
               << manifest_read.message << L'\n';
  }
  if (!manifest_read.Succeeded() || !recovered_manifest.IsReady() ||
      recovered_manifest.video.track_id != kVideoTrackId ||
      recovered_manifest.video.h264_avcc_extradata.size() !=
          video_metadata.h264.avcc_extradata.size() ||
      recovered_manifest.video.source_width != video_metadata.source_width ||
      recovered_manifest.video.output_width != video_metadata.output_width) {
    std::wcerr << L"  Session manifest readback was not ready.\n";
    return 1;
  }

  int64_t export_end_ns = 0;
  for (const auto& packet : packet_store_index) {
    if (packet.metadata.track_id == kVideoTrackId &&
        packet.metadata.codec_id == olouie::record::CodecId::H264) {
      export_end_ns = std::max(export_end_ns, packet.EndPtsNs());
    }
  }
  if (export_end_ns <= 0) {
    std::wcerr << L"  PacketStore did not contain a usable video range.\n";
    return 1;
  }

  bootstrap.packet_store.Close();

  olouie::record::VideoExportPlanOptions export_options;
  export_options.requested_start_ns = 0;
  export_options.requested_end_ns = export_end_ns;
  olouie::record::VideoExportPlan export_plan;
  const auto export_plan_result =
      olouie::record::BuildRecoveredVideoExportPlan(
          video_metadata.packet_store_session_dir, export_options,
          &export_plan);
  std::wcout << L"  recovered video export plan status: "
             << olouie::record::VideoExportPlanStatusName(
                    export_plan_result.status)
             << L'\n';
  if (!export_plan_result.message.empty()) {
    std::wcout << L"  recovered video export plan message: "
               << export_plan_result.message << L'\n';
  }
  if (!export_plan_result.Succeeded() || !export_plan.IsReady() ||
      export_plan.video.track_id != kVideoTrackId ||
      export_plan.video.h264_avcc_extradata.size() !=
          video_metadata.h264.avcc_extradata.size() ||
      export_plan.video.width != video_metadata.output_width ||
      export_plan.mux_plan.packets.size() != packet_store_index.size()) {
    std::wcerr << L"  Recovered video export plan was not ready.\n";
    return 1;
  }

  olouie::record::Mp4MuxRequest mp4_request;
  const auto mp4_request_result =
      olouie::record::BuildVideoMp4MuxRequest(
          export_plan, video_metadata.packet_store_session_dir / L"exports" /
                           L"wgc-smoke.tmp",
          video_metadata.packet_store_session_dir / L"exports" /
              L"wgc-smoke.mp4",
          false, &mp4_request);
  std::wcout << L"  video MP4 mux request status: "
             << olouie::record::Mp4MuxStatusName(mp4_request_result.status)
             << L'\n';
  if (!mp4_request_result.message.empty()) {
    std::wcout << L"  video MP4 mux request message: "
               << mp4_request_result.message << L'\n';
  }
  if (!mp4_request_result.Succeeded() ||
      !mp4_request.video_track.IsReady() ||
      mp4_request.packet_file_path != export_plan.packet_file_path ||
      mp4_request.video_track.track_id != kVideoTrackId ||
      mp4_request.video_track.avcc_extradata.size() !=
          video_metadata.h264.avcc_extradata.size()) {
    std::wcerr << L"  Video MP4 mux request was not ready.\n";
    return 1;
  }

  const auto backend = olouie::record::Mp4Muxer::BackendAvailability();
  std::wcout << L"  MP4 backend status: "
             << olouie::record::Mp4MuxBackendStatusName(backend.status)
             << L'\n';
  if (!backend.message.empty()) {
    std::wcout << L"  MP4 backend message: " << backend.message << L'\n';
  }
  std::wcout << L"  MP4 backend dynamic libraries:";
  for (const auto& library : backend.required_libraries) {
    std::wcout << L" " << library;
  }
  std::wcout << L'\n';

  olouie::record::Mp4MuxStreamSetupStats stream_setup_stats;
  const auto stream_setup_result =
      olouie::record::Mp4Muxer::ValidateVideoStreamSetup(mp4_request,
                                                         &stream_setup_stats);
  std::wcout << L"  MP4 stream setup status: "
             << olouie::record::Mp4MuxStatusName(
                    stream_setup_result.status)
             << L'\n';
  if (!stream_setup_result.message.empty()) {
    std::wcout << L"  MP4 stream setup message: "
               << stream_setup_result.message << L'\n';
  }
  if (backend.Available()) {
    if (!stream_setup_result.Succeeded() || !stream_setup_stats.IsReady() ||
        stream_setup_stats.extradata_bytes !=
            mp4_request.video_track.avcc_extradata.size()) {
      std::wcerr << L"  MP4 stream setup validation was not ready.\n";
      return 1;
    }
  } else if (stream_setup_result.status !=
                 olouie::record::Mp4MuxStatus::BackendUnavailable ||
             stream_setup_stats.IsReady()) {
    std::wcerr << L"  MP4 stream setup did not report unavailable backend.\n";
    return 1;
  }

  olouie::record::Mp4MuxPayloadReadStats payload_read_stats;
  const auto payload_read_result =
      olouie::record::Mp4Muxer::DryRunPayloadRead(mp4_request,
                                                  &payload_read_stats);
  std::wcout << L"  MP4 payload dry run status: "
             << olouie::record::Mp4MuxStatusName(payload_read_result.status)
             << L'\n';
  if (!payload_read_result.message.empty()) {
    std::wcout << L"  MP4 payload dry run message: "
               << payload_read_result.message << L'\n';
  }
  if (!payload_read_result.Succeeded() ||
      payload_read_stats.packet_count != export_plan.mux_plan.packets.size() ||
      payload_read_stats.video_packet_count !=
          export_plan.mux_plan.packets.size() ||
      payload_read_stats.video_payload_byte_count == 0) {
    std::wcerr << L"  MP4 payload dry run was not ready.\n";
    return 1;
  }

  const olouie::record::Mp4Muxer mp4_muxer;
  const auto mp4_write_result = mp4_muxer.WriteMp4(mp4_request);
  std::wcout << L"  MP4 write status: "
             << olouie::record::Mp4MuxStatusName(mp4_write_result.status)
             << L'\n';
  if (!mp4_write_result.message.empty()) {
    std::wcout << L"  MP4 write message: " << mp4_write_result.message
               << L'\n';
  }
  if (backend.Available()) {
    if (!mp4_write_result.Succeeded() ||
        !std::filesystem::exists(mp4_request.final_output_path) ||
        std::filesystem::exists(mp4_request.temp_output_path)) {
      std::wcerr << L"  MP4 write did not produce a finalized file.\n";
      return 1;
    }
  } else if (mp4_write_result.status !=
             olouie::record::Mp4MuxStatus::BackendUnavailable) {
    std::wcerr << L"  MP4 write did not report unavailable backend.\n";
    return 1;
  }

  std::wcout << L"  converted frames: "
             << video_chain->session_stats().converted_frame_count << L'\n'
             << L"  submitted frames: "
             << video_chain->session_stats().submitted_frame_count << L'\n'
             << L"  drained packets: "
             << video_chain->session_stats().drained_packet_count << L'\n'
             << L"  appended packets: "
             << video_chain->session_stats().appended_packet_count << L'\n'
             << L"  encoded packets: " << packet_store_index.size() << L'\n'
             << L"  encoded bytes: " << total_bytes << L'\n'
             << L"  PacketStore video track: " << kVideoTrackId << L'\n'
             << L"  PacketStore H.264 config ready: "
             << (video_chain->session_config().IsReady() ? L"yes" : L"no")
             << L'\n'
             << L"  PacketStore AVCC extradata bytes: "
             << video_chain->session_config().avcc_extradata.size() << L'\n'
             << L"  metadata encoder: " << video_metadata.encoder_name << L'\n'
             << L"  metadata source: " << video_metadata.source_width << L"x"
             << video_metadata.source_height << L'\n'
             << L"  metadata output: " << video_metadata.output_width << L"x"
             << video_metadata.output_height << L'\n'
             << L"  metadata AVCC extradata bytes: "
             << video_metadata.h264.avcc_extradata.size() << L'\n'
             << L"  recovered export video packets: "
             << export_plan.mux_plan.packets.size() << L'\n'
             << L"  recovered export source duration ns: "
             << (export_plan.mux_plan.source_end_ns -
                 export_plan.mux_plan.source_start_ns)
             << L'\n'
             << L"  MP4 request video: " << mp4_request.video_track.width
             << L"x" << mp4_request.video_track.height << L" @ "
             << mp4_request.video_track.fps_numerator << L"/"
             << mp4_request.video_track.fps_denominator << L" fps\n"
             << L"  MP4 request AVCC extradata bytes: "
             << mp4_request.video_track.avcc_extradata.size() << L'\n'
             << L"  MP4 stream setup ready: "
             << (stream_setup_stats.IsReady() ? L"yes" : L"no") << L'\n'
             << L"  MP4 stream setup AVCC extradata bytes: "
             << stream_setup_stats.extradata_bytes << L'\n'
             << L"  MP4 payload dry run packets: "
             << payload_read_stats.packet_count << L'\n'
             << L"  MP4 payload dry run bytes: "
             << payload_read_stats.payload_byte_count << L'\n'
             << L"  MP4 write output: "
             << mp4_request.final_output_path.native() << L'\n'
             << L"  session manifest path: "
             << olouie::record::SessionManifestPath(
                    video_metadata.packet_store_session_dir)
                    .native()
             << L'\n'
             << L"  PacketStore packets: " << packet_store_index.size()
             << L'\n'
             << L"  PacketStore path: "
             << bootstrap.packet_store.session_dir().native() << L'\n';

  return packet_store_index.size() >= kWgcSubmitFrameCount ? 0 : 1;
}

int RunH264Session(const olouie::encode::MfHardwareH264EncoderConfig& config,
                   bool submit_synthetic_frame,
                   bool submit_bgra_frame) {
  olouie::encode::MfHardwareH264EncoderProbeOptions options;
  options.include_local_mfts = true;

  std::wcout << (submit_bgra_frame
                     ? L"Submitting generated BGRA frames through GPU "
                       L"conversion to Media Foundation hardware H.264 "
                       L"session...\n"
                 : submit_synthetic_frame
                     ? L"Submitting synthetic NV12 frame to Media Foundation "
                       L"hardware H.264 session...\n"
                     : L"Configuring Media Foundation hardware H.264 "
                       L"session...\n")
             << L"  requested: " << config.width << L"x" << config.height
             << L" @ " << config.fps_numerator << L"/"
             << config.fps_denominator << L" fps, "
             << (config.bitrate_bps / 1000000u) << L" Mbps, GOP "
             << config.gop_seconds << L"s, B-frames "
             << config.max_b_frames << L", performance "
             << olouie::performance::CapturePerformanceModeName(
                    config.performance_mode)
             << L'\n';

  const auto monitors = olouie::graphics::EnumerateMonitors();
  const auto* monitor = olouie::graphics::FindPrimaryMonitor(monitors);
  if (monitor == nullptr) {
    std::wcerr << L"No monitor is available for D3D11 device creation.\n";
    return 1;
  }

  std::wstring d3d_error;
  auto d3d =
      olouie::graphics::D3D11DeviceContext::CreateForMonitor(monitor->handle,
                                                             &d3d_error);
  if (!d3d.IsValid()) {
    std::wcerr << L"D3D11 device creation failed: " << d3d_error << L'\n';
    return 1;
  }
  std::wcout << L"  D3D11 adapter: " << d3d.adapter_description() << L'\n';

  olouie::encode::MfHardwareH264EncoderSession session;
  const auto result = session.Initialize(config, options, d3d.device());
  std::wcout << L"  status: "
             << olouie::encode::MfHardwareH264EncoderSessionStatusName(
                    result.status)
             << L'\n';
  if (!result.message.empty()) {
    std::wcout << L"  message: " << result.message << L'\n';
  }

  const auto& info = result.info;
  if (!info.encoder.name.empty()) {
    std::wcout << L"  encoder: " << info.encoder.name << L'\n';
  }
  if (!info.encoder.clsid.empty()) {
    std::wcout << L"  clsid: " << info.encoder.clsid << L'\n';
  }

  std::wcout << L"  media type: " << info.media_type.width << L"x"
             << info.media_type.height << L" @ "
             << info.media_type.fps_numerator << L"/"
             << info.media_type.fps_denominator << L" fps, "
             << info.media_type.bitrate_bps << L" bps, GOP "
             << info.media_type.gop_frame_count << L" frames, profile "
             << info.media_type.h264_profile << L'\n'
             << L"  D3D11 aware: "
             << (info.d3d11_aware ? L"yes" : L"no") << L'\n'
             << L"  D3D11 device supplied: "
             << (info.d3d11_device_supplied ? L"yes" : L"no") << L'\n'
             << L"  DXGI manager created: "
             << (info.device_manager_created ? L"yes" : L"no") << L'\n'
             << L"  DXGI manager reset: "
             << (info.device_manager_reset ? L"yes" : L"no") << L'\n'
             << L"  DXGI manager attached: "
             << (info.device_manager_attached ? L"yes" : L"no") << L'\n'
             << L"  async transform: "
             << (info.async_transform ? L"yes" : L"no") << L'\n'
             << L"  async unlocked: "
             << (info.async_unlocked ? L"yes" : L"no") << L'\n'
             << L"  codec API: "
             << (info.codec_api_available ? L"yes" : L"no") << L'\n'
             << L"  output type configured: "
             << (info.output_type_configured ? L"yes" : L"no") << L'\n'
             << L"  input type configured: "
             << (info.input_type_configured ? L"yes" : L"no") << L'\n';

  if (!info.codec_settings.empty()) {
    std::wcout << L"  codec settings:\n";
    for (const auto& setting : info.codec_settings) {
      PrintCodecSetting(setting);
    }
  }

  if (submit_synthetic_frame && result.Succeeded()) {
    constexpr uint32_t kVideoTrackId = 1;
    const int64_t frame_duration_ns = FrameDurationNs(config);
    std::vector<olouie::encode::MfHardwareH264EncodedPacket> encoded_packets;
    uint64_t events_checked = 0;
    bool saw_output_event = false;
    bool saw_need_input_event = false;

    std::wstring packet_store_error;
    const auto session_dir = MakeManualH264PacketStoreSessionDir();
    std::filesystem::remove_all(session_dir.parent_path());

    const std::array tracks{
        olouie::record::TrackDefinition{kVideoTrackId,
                                        olouie::record::CodecId::H264},
    };
    auto store = olouie::record::PacketStore::Create(
        session_dir, tracks, &packet_store_error);
    if (!store.IsWritable()) {
      std::wcerr << L"  PacketStore create failed: " << packet_store_error
                 << L'\n';
      return 1;
    }

    olouie::encode::SyntheticVideoRecordingSession video_session(
        olouie::encode::SyntheticVideoRecordingSessionOptions{kVideoTrackId,
                                                              3000});
    const auto prepare = video_session.Prepare(&session, d3d.device(), &store);
    std::wcout << L"  video session prepare status: "
               << olouie::encode::VideoRecordingSessionStatusName(
                      prepare.status)
               << L'\n';
    if (!prepare.message.empty()) {
      std::wcout << L"  video session prepare message: " << prepare.message
                 << L'\n';
    }
    if (!prepare.Succeeded()) {
      return 1;
    }

    for (uint32_t frame_index = 0; frame_index < kSyntheticSubmitFrameCount;
         ++frame_index) {
      auto frame_result = video_session.SubmitGeneratedFrame(
          static_cast<int64_t>(frame_index) * frame_duration_ns,
          frame_duration_ns);
      const auto& submit_result = frame_result.submit_result;
      std::wcout << L"  synthetic submit " << frame_index << L" status: "
                 << olouie::encode::MfHardwareH264EncoderFrameSubmitStatusName(
                        submit_result.status)
                 << L'\n';
      if (!submit_result.message.empty()) {
        std::wcout << L"  synthetic submit " << frame_index
                   << L" message: " << submit_result.message << L'\n';
      }

      std::wcout << L"    synthetic texture created: "
                 << (submit_result.synthetic_texture_created ? L"yes" : L"no")
                 << L'\n'
                 << L"    MF sample created: "
                 << (submit_result.sample_created ? L"yes" : L"no") << L'\n'
                 << L"    stream started: "
                 << (submit_result.stream_started ? L"yes" : L"no") << L'\n'
                 << L"    input submitted: "
                 << (submit_result.input_submitted ? L"yes" : L"no") << L'\n'
                 << L"    submitted input frames: "
                 << submit_result.submitted_input_frames << L'\n';

      if (!submit_result.Succeeded() || !frame_result.Succeeded()) {
        if (!frame_result.message.empty()) {
          std::wcerr << L"  video session frame " << frame_index
                     << L" failed: " << frame_result.message << L'\n';
        }
        return 1;
      }
      if (session.pending_input_sample_count() != 0 ||
          session.info().pending_input_samples != 0) {
        std::wcerr << L"  hardware encoder retained a drained synthetic "
                      L"input sample.\n";
        return 1;
      }

      auto& available = frame_result.drain_result;
      events_checked += available.events_checked;
      saw_output_event = saw_output_event || available.saw_have_output_event;
      saw_need_input_event =
          saw_need_input_event || available.saw_need_input_event;
      std::wcout << L"  synthetic available drain " << frame_index
                 << L" status: "
                 << olouie::encode::MfHardwareH264EncoderDrainStatusName(
                        available.status)
                 << L'\n';
      if (!available.message.empty()) {
        std::wcout << L"  synthetic available drain " << frame_index
                   << L" message: " << available.message << L'\n';
      }
      if (!available.Succeeded()) {
        return 1;
      }
      for (auto& packet : available.packets) {
        encoded_packets.push_back(packet);
      }
    }

    size_t total_bytes = 0;
    for (const auto& packet : encoded_packets) {
      total_bytes += packet.data.size();
    }

    std::wcout << L"  drain command sent: no\n"
               << L"  events checked: " << events_checked << L'\n'
               << L"  saw output event: "
               << (saw_output_event ? L"yes" : L"no")
               << L'\n'
               << L"  saw need-input event: "
               << (saw_need_input_event ? L"yes" : L"no")
               << L'\n'
               << L"  encoded packets: " << encoded_packets.size()
               << L'\n'
               << L"  encoded bytes: " << total_bytes << L'\n';
    if (encoded_packets.size() != kSyntheticSubmitFrameCount) {
      std::wcerr << L"  expected " << kSyntheticSubmitFrameCount
                 << L" encoded packets from synthetic frame batch.\n";
      return 1;
    }
    for (size_t index = 0; index < encoded_packets.size(); ++index) {
      const auto& packet = encoded_packets[index];
      std::wcout << L"    packet " << index << L": "
                 << packet.data.size() << L" bytes, pts "
                 << packet.pts_ns << L" ns, duration "
                 << packet.duration_ns << L" ns, keyframe "
                 << (packet.keyframe ? L"yes" : L"no") << L'\n'
                 << L"      packet format: "
                 << olouie::encode::MfHardwareH264PacketFormatName(
                        packet.bitstream.packet_format)
                 << L", NAL units " << packet.bitstream.nal_unit_count
                 << L", SPS " << packet.bitstream.sps_count << L", PPS "
                 << packet.bitstream.pps_count << L", IDR "
                 << packet.bitstream.idr_count << L'\n'
                 << L"      SPS bytes: "
                 << packet.bitstream.config.sps.size() << L", PPS bytes: "
                 << packet.bitstream.config.pps.size() << L'\n'
                 << L"      AVCC extradata bytes: "
                 << packet.bitstream.config.avcc_extradata.size() << L'\n'
                 << L"      MP4 extradata ready: "
                 << (packet.bitstream.mp4_extradata_ready ? L"yes" : L"no")
                 << L'\n';
    }

    const auto packet_store_index = store.SnapshotIndex();
    if (packet_store_index.size() != encoded_packets.size()) {
      std::wcerr << L"  PacketStore packet count did not match drain output.\n";
      return 1;
    }
    for (size_t index = 1; index < packet_store_index.size(); ++index) {
      if (packet_store_index[index].metadata.pts_ns <
          packet_store_index[index - 1].metadata.pts_ns) {
        std::wcerr << L"  PacketStore H.264 packets are not timestamp ordered.\n";
        return 1;
      }
    }
    std::wcout << L"  PacketStore video track: " << kVideoTrackId << L'\n'
               << L"  PacketStore H.264 config ready: "
               << (video_session.config().IsReady() ? L"yes" : L"no") << L'\n'
               << L"  PacketStore AVCC extradata bytes: "
               << video_session.config().avcc_extradata.size() << L'\n'
               << L"  PacketStore packets: " << packet_store_index.size()
               << L'\n'
               << L"  PacketStore path: " << session_dir.native() << L'\n';

    return 0;
  }

  if (submit_bgra_frame && result.Succeeded()) {
    constexpr uint32_t kVideoTrackId = 1;
    const int64_t frame_duration_ns = FrameDurationNs(config);
    std::vector<olouie::encode::MfHardwareH264EncodedPacket> encoded_packets;
    uint64_t events_checked = 0;
    bool saw_output_event = false;
    bool saw_need_input_event = false;

    std::wstring packet_store_error;
    const auto session_dir = MakeManualH264PacketStoreSessionDir();
    std::filesystem::remove_all(session_dir.parent_path());

    const std::array tracks{
        olouie::record::TrackDefinition{kVideoTrackId,
                                        olouie::record::CodecId::H264},
    };
    auto store = olouie::record::PacketStore::Create(
        session_dir, tracks, &packet_store_error);
    if (!store.IsWritable()) {
      std::wcerr << L"  PacketStore create failed: " << packet_store_error
                 << L'\n';
      return 1;
    }

    olouie::encode::BgraVideoRecordingSession video_session(
        olouie::encode::BgraVideoRecordingSessionOptions{
            kVideoTrackId, 3000, config.width, config.height});
    const auto prepare = video_session.Prepare(
        &session, d3d.device(), d3d.immediate_context(), &store);
    std::wcout << L"  BGRA video session prepare status: "
               << olouie::encode::VideoRecordingSessionStatusName(
                      prepare.status)
               << L'\n';
    if (!prepare.message.empty()) {
      std::wcout << L"  BGRA video session prepare message: "
                 << prepare.message << L'\n';
    }
    if (!prepare.Succeeded()) {
      return 1;
    }

    const auto& convert_plan = video_session.conversion_plan();
    std::wcout << L"  conversion: " << convert_plan.source_width << L"x"
               << convert_plan.source_height << L" BGRA -> "
               << convert_plan.output_width << L"x"
               << convert_plan.output_height << L" NV12\n";

    const UINT source_bind_candidates[] = {
        D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        0u,
        D3D11_BIND_DECODER,
    };
    UINT selected_source_bind = std::numeric_limits<UINT>::max();

    for (uint32_t frame_index = 0; frame_index < kSyntheticSubmitFrameCount;
         ++frame_index) {
      olouie::encode::BgraVideoRecordingSessionResult frame_result;
      winrt::com_ptr<ID3D11Texture2D> source_texture;

      if (selected_source_bind == std::numeric_limits<UINT>::max()) {
        for (const UINT bind_flags : source_bind_candidates) {
          source_texture = nullptr;
          const HRESULT texture_result = CreateGeneratedBgraTexture(
              d3d.device(), config.width, config.height, bind_flags,
              frame_index, source_texture.put());
          if (FAILED(texture_result)) {
            continue;
          }

          frame_result = video_session.SubmitBgraFrame(
              source_texture.get(),
              static_cast<int64_t>(frame_index) * frame_duration_ns,
              frame_duration_ns);
          if (frame_result.Succeeded()) {
            selected_source_bind = bind_flags;
            break;
          }
          if (frame_result.status !=
              olouie::encode::VideoRecordingSessionStatus::ConvertFailed) {
            break;
          }
        }
      } else {
        const HRESULT texture_result = CreateGeneratedBgraTexture(
            d3d.device(), config.width, config.height, selected_source_bind,
            frame_index, source_texture.put());
        if (FAILED(texture_result)) {
          std::wcerr << L"  generated BGRA texture creation failed.\n";
          return 1;
        }
        frame_result = video_session.SubmitBgraFrame(
            source_texture.get(),
            static_cast<int64_t>(frame_index) * frame_duration_ns,
            frame_duration_ns);
      }

      std::wcout << L"  BGRA submit " << frame_index << L" status: "
                 << olouie::encode::VideoRecordingSessionStatusName(
                        frame_result.status)
                 << L'\n';
      if (!frame_result.message.empty()) {
        std::wcout << L"  BGRA submit " << frame_index
                   << L" message: " << frame_result.message << L'\n';
      }

      const auto& submit_result = frame_result.submit_result;
      std::wcout << L"    MF sample created: "
                 << (submit_result.sample_created ? L"yes" : L"no") << L'\n'
                 << L"    stream started: "
                 << (submit_result.stream_started ? L"yes" : L"no") << L'\n'
                 << L"    input submitted: "
                 << (submit_result.input_submitted ? L"yes" : L"no") << L'\n'
                 << L"    submitted input frames: "
                 << submit_result.submitted_input_frames << L'\n';

      if (!frame_result.Succeeded()) {
        return 1;
      }
      if (session.pending_input_sample_count() != 0 ||
          session.info().pending_input_samples != 0) {
        std::wcerr << L"  hardware encoder retained a drained BGRA input "
                      L"sample.\n";
        return 1;
      }

      auto& available = frame_result.drain_result;
      events_checked += available.events_checked;
      saw_output_event = saw_output_event || available.saw_have_output_event;
      saw_need_input_event =
          saw_need_input_event || available.saw_need_input_event;
      if (!available.Succeeded()) {
        return 1;
      }
      for (auto& packet : available.packets) {
        encoded_packets.push_back(packet);
      }
    }

    size_t total_bytes = 0;
    for (const auto& packet : encoded_packets) {
      total_bytes += packet.data.size();
    }

    std::wcout << L"  selected BGRA source bind flags: 0x" << std::hex
               << selected_source_bind << std::dec << L'\n'
               << L"  converted frames: "
               << video_session.stats().converted_frame_count << L'\n'
               << L"  submitted frames: "
               << video_session.stats().submitted_frame_count << L'\n'
               << L"  events checked: " << events_checked << L'\n'
               << L"  saw output event: "
               << (saw_output_event ? L"yes" : L"no") << L'\n'
               << L"  saw need-input event: "
               << (saw_need_input_event ? L"yes" : L"no") << L'\n'
               << L"  encoded packets: " << encoded_packets.size() << L'\n'
               << L"  encoded bytes: " << total_bytes << L'\n';
    if (encoded_packets.size() != kSyntheticSubmitFrameCount) {
      std::wcerr << L"  expected " << kSyntheticSubmitFrameCount
                 << L" encoded packets from BGRA frame batch.\n";
      return 1;
    }

    const auto packet_store_index = store.SnapshotIndex();
    if (packet_store_index.size() != encoded_packets.size()) {
      std::wcerr << L"  PacketStore packet count did not match drain output.\n";
      return 1;
    }
    std::wcout << L"  PacketStore video track: " << kVideoTrackId << L'\n'
               << L"  PacketStore H.264 config ready: "
               << (video_session.config().IsReady() ? L"yes" : L"no")
               << L'\n'
               << L"  PacketStore AVCC extradata bytes: "
               << video_session.config().avcc_extradata.size() << L'\n'
               << L"  PacketStore packets: " << packet_store_index.size()
               << L'\n'
               << L"  PacketStore path: " << session_dir.native() << L'\n';

    return 0;
  }

  return result.Succeeded() ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool default_probe = argc == 1;
  const bool h264_probe =
      argc >= 2 && std::wstring(argv[1]) == L"--h264-probe";
  const bool h264_session =
      argc >= 2 && std::wstring(argv[1]) == L"--h264-session";
  const bool h264_submit =
      argc >= 2 && std::wstring(argv[1]) == L"--h264-submit";
  const bool h264_bgra_submit =
      argc >= 2 && std::wstring(argv[1]) == L"--h264-bgra-submit";
  const bool h264_wgc_submit =
      argc >= 2 && std::wstring(argv[1]) == L"--h264-wgc-submit";
  if (!default_probe && !h264_probe && !h264_session && !h264_submit &&
      !h264_bgra_submit && !h264_wgc_submit) {
    PrintUsage();
    return 2;
  }

  olouie::encode::MfHardwareH264EncoderConfig config;
  int parse_argc = argc;
  const bool capture_first =
      argc >= 3 && std::wstring(argv[argc - 1]) == L"--capture-first";
  if (capture_first) {
    --parse_argc;
  }
  std::chrono::milliseconds wgc_duration(0);
  if (h264_wgc_submit) {
    if (!ParseWgcSubmitConfig(parse_argc, argv, &wgc_duration, &config)) {
      PrintUsage();
      return 2;
    }
  } else if (!ParseConfig(parse_argc, argv, &config)) {
    PrintUsage();
    return 2;
  }
  if (capture_first) {
    config.performance_mode =
        olouie::performance::CapturePerformanceMode::CaptureFirst;
  }

  ComApartment com;
  if (!com.Initialize()) {
    std::wcerr << L"COM initialization failed.\n";
    return 1;
  }

  if (h264_wgc_submit) {
    return RunH264WgcSession(config, wgc_duration);
  }

  if (h264_session || h264_submit || h264_bgra_submit) {
    return RunH264Session(config, h264_submit, h264_bgra_submit);
  }

  return RunH264Probe(config);
}
