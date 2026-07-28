#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "diagnostics/DiagnosticsSnapshot.h"
#include "record/ActiveRecordingBookmark.h"
#include "record/ActiveRecordingClip.h"
#include "record/Bookmark.h"
#include "record/ClipExportQueue.h"
#include "record/DiskWriteFault.h"
#include "record/Mp4Muxer.h"
#include "record/MuxPlan.h"
#include "record/PacketStore.h"
#include "record/RecordingRecovery.h"
#include "record/SessionClock.h"
#include "record/SessionManifest.h"
#include "record/Timebase.h"
#include "record/VideoExportPlan.h"
#include "record/VideoRecorderPipeline.h"
#include "record/VideoRecorderSession.h"

namespace {

constexpr int64_t kMs = 1000000;

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

std::wstring ReadWideEnv(const wchar_t* name) {
  size_t required = 0;
  if (_wgetenv_s(&required, nullptr, 0, name) != 0 || required <= 1) {
    return {};
  }

  std::wstring value(required, L'\0');
  if (_wgetenv_s(&required, value.data(), value.size(), name) != 0 ||
      required == 0) {
    return {};
  }

  if (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

std::filesystem::path RecordTestOutputDir() {
  const auto output_dir = ReadWideEnv(L"OLOUIE_RECORD_TEST_OUTPUT_DIR");
  if (output_dir.empty()) {
    return {};
  }
  return std::filesystem::path(output_dir);
}

std::filesystem::path RecordTestRoot(std::wstring_view name) {
  const auto output_dir = RecordTestOutputDir();
  if (!output_dir.empty()) {
    return output_dir / std::wstring(name);
  }
  return std::filesystem::temp_directory_path() / std::wstring(name);
}

void RemoveRecordTestRootIfTemporary(const std::filesystem::path& root) {
  if (RecordTestOutputDir().empty()) {
    std::filesystem::remove_all(root);
  }
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    bytes[index] = static_cast<std::byte>(text[index]);
  }
  return bytes;
}

std::vector<std::byte> Bytes(std::initializer_list<uint8_t> values) {
  std::vector<std::byte> bytes;
  bytes.reserve(values.size());
  for (const uint8_t value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

bool PayloadEquals(const std::vector<std::byte>& payload,
                   std::string_view expected) {
  if (payload.size() != expected.size()) {
    return false;
  }

  for (size_t index = 0; index < expected.size(); ++index) {
    if (payload[index] != static_cast<std::byte>(expected[index])) {
      return false;
    }
  }

  return true;
}

olouie::record::PacketMetadata VideoPacket(int64_t pts_ns, bool keyframe) {
  return {1,
          olouie::record::CodecId::H264,
          static_cast<uint16_t>(keyframe ? olouie::record::PacketFlagKeyframe
                                         : olouie::record::PacketFlagNone),
          pts_ns,
          pts_ns,
          33 * kMs};
}

olouie::record::PacketMetadata AudioPacket(int64_t pts_ns,
                                           int64_t duration_ns,
                                           uint32_t track_id = 2) {
  return {track_id, olouie::record::CodecId::Aac,
          olouie::record::PacketFlagNone, pts_ns, pts_ns, duration_ns};
}

bool Append(olouie::record::PacketStore* store,
            const olouie::record::PacketMetadata& metadata,
            std::string_view payload_text) {
  auto payload = Bytes(payload_text);
  std::wstring error;
  if (!store->AppendPacket(metadata, std::span<const std::byte>(payload),
                           &error)) {
    std::wcerr << L"Append failed: " << error << L'\n';
    return false;
  }
  return true;
}

bool Append(olouie::record::PacketStore* store,
            const olouie::record::PacketMetadata& metadata,
            std::initializer_list<uint8_t> payload_bytes) {
  auto payload = Bytes(payload_bytes);
  std::wstring error;
  if (!store->AppendPacket(metadata, std::span<const std::byte>(payload),
                           &error)) {
    std::wcerr << L"Append failed: " << error << L'\n';
    return false;
  }
  return true;
}

olouie::record::SessionManifest MakeReadyVideoManifest(
    const std::filesystem::path& session_dir,
    uint32_t video_track_id) {
  olouie::record::SessionManifest manifest;
  manifest.session_dir = session_dir;
  manifest.packet_file_path = session_dir / L"packets.dat";
  manifest.video.track_id = video_track_id;
  manifest.video.codec_id = olouie::record::CodecId::H264;
  manifest.video.h264_packet_format = L"annex_b";
  manifest.video.h264_sps = {0x67, 0x42, 0x00, 0x1f};
  manifest.video.h264_pps = {0x68, 0xce, 0x06};
  manifest.video.h264_avcc_extradata = {
      0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67,
      0x42, 0x00, 0x1f, 0x01, 0x00, 0x03, 0x68, 0xce, 0x06,
  };
  manifest.video.requested_width = 1920;
  manifest.video.requested_height = 1080;
  manifest.video.requested_fps_numerator = 60;
  manifest.video.requested_fps_denominator = 1;
  manifest.video.requested_bitrate_bps = 20000000;
  manifest.video.requested_gop_seconds = 2.0;
  manifest.video.requested_max_b_frames = 0;
  manifest.video.encoder_name = L"Fake Hardware H.264 Encoder";
  manifest.video.encoder_clsid = L"{fake-clsid}";
  manifest.video.encoder_enumeration_flags = 7;
  manifest.video.media_width = 1920;
  manifest.video.media_height = 1080;
  manifest.video.media_fps_numerator = 60;
  manifest.video.media_fps_denominator = 1;
  manifest.video.media_bitrate_bps = 20000000;
  manifest.video.media_gop_frame_count = 120;
  manifest.video.media_h264_profile = 100;
  manifest.video.media_max_b_frames = 0;
  manifest.video.d3d11_aware = true;
  manifest.video.device_manager_attached = true;
  manifest.video.async_transform = true;
  manifest.video.async_unlocked = true;
  manifest.video.codec_api_available = true;
  manifest.video.monitor_device_name = L"DISPLAY1";
  manifest.video.monitor_primary = true;
  manifest.video.monitor_left = 0;
  manifest.video.monitor_top = 0;
  manifest.video.monitor_right = 1920;
  manifest.video.monitor_bottom = 1080;
  manifest.video.source_width = 1920;
  manifest.video.source_height = 1080;
  manifest.video.output_width = 1920;
  manifest.video.output_height = 1080;
  return manifest;
}

olouie::record::AudioTrackSessionManifest MakeReadyAudioManifestTrack(
    uint32_t track_id,
    std::wstring source_kind = L"system_loopback",
    std::wstring name = L"System loopback") {
  olouie::record::AudioTrackSessionManifest audio;
  audio.track_id = track_id;
  audio.codec_id = olouie::record::CodecId::Aac;
  audio.source_kind = std::move(source_kind);
  audio.name = std::move(name);
  audio.sample_rate = 48000;
  audio.channel_count = 2;
  audio.bitrate_bps = 192000;
  audio.aac_frame_samples = 1024;
  audio.aac_payload_type = 0;
  audio.aac_profile_level_indication = 0x29;
  audio.aac_audio_object_type = 2;
  audio.aac_audio_specific_config = {0x11, 0x90};
  audio.encoder_name = L"Fake AAC Encoder";
  return audio;
}

olouie::record::Mp4H264VideoTrack MakeReadyMp4VideoTrack(
    uint32_t video_track_id) {
  olouie::record::Mp4H264VideoTrack video;
  video.track_id = video_track_id;
  video.codec_id = olouie::record::CodecId::H264;
  video.width = 1920;
  video.height = 1080;
  video.fps_numerator = 60;
  video.fps_denominator = 1;
  video.packet_format = L"annex_b";
  video.sps = {0x67, 0x42, 0x00, 0x1f};
  video.pps = {0x68, 0xce, 0x06};
  video.avcc_extradata = {
      0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67,
      0x42, 0x00, 0x1f, 0x01, 0x00, 0x03, 0x68, 0xce, 0x06,
  };
  return video;
}

olouie::record::Mp4AacAudioTrack MakeReadyMp4AudioTrack(
    uint32_t track_id) {
  const auto manifest = MakeReadyAudioManifestTrack(track_id);
  olouie::record::Mp4AacAudioTrack audio;
  audio.track_id = manifest.track_id;
  audio.codec_id = manifest.codec_id;
  audio.source_kind = manifest.source_kind;
  audio.name = manifest.name;
  audio.sample_rate = manifest.sample_rate;
  audio.channel_count = manifest.channel_count;
  audio.bitrate_bps = manifest.bitrate_bps;
  audio.frame_samples = manifest.aac_frame_samples;
  audio.payload_type = manifest.aac_payload_type;
  audio.profile_level_indication =
      manifest.aac_profile_level_indication;
  audio.audio_object_type = manifest.aac_audio_object_type;
  audio.audio_specific_config = manifest.aac_audio_specific_config;
  audio.encoder_name = manifest.encoder_name;
  return audio;
}

int TestTimebase() {
  std::wstring error;
  const auto timebase = olouie::record::Timebase::FromQpc(10000000, 100, &error);
  if (!timebase.IsValid()) {
    std::wcerr << L"Timebase creation failed: " << error << L'\n';
    return 1;
  }

  if (timebase.QpcToNs(100) != 0 ||
      timebase.QpcToNs(10000100) != 1000000000 ||
      timebase.QpcDurationToNs(5000000) != 500000000 ||
      timebase.SamplesToNs(48000, 48000) != 1000000000) {
    return Fail("Timebase conversion results changed unexpectedly.");
  }

  const auto invalid = olouie::record::Timebase::FromQpc(0, 0, &error);
  if (invalid.IsValid()) {
    return Fail("Invalid QPC frequency should not create a valid timebase.");
  }

  return 0;
}

int TestSessionClock() {
  olouie::record::SessionClock clock;
  std::wstring error;
  if (!olouie::record::BuildSessionClock(10000000, 123456789, &clock,
                                         &error) ||
      !clock.IsValid() || clock.origin_100ns != 123456789 ||
      clock.origin_ns != 12345678900LL) {
    return Fail("Session clock should preserve an already 100 ns QPC scale.");
  }

  const auto video_timebase = olouie::record::Timebase::FromQpc(
      olouie::record::kSystemRelativeTimestampFrequency,
      clock.origin_100ns, &error);
  const int64_t offset_100ns = 25000;
  const int64_t video_pts_ns =
      video_timebase.QpcToNs(clock.origin_100ns + offset_100ns);
  const int64_t audio_pts_ns =
      (clock.origin_ns + offset_100ns * 100) - clock.origin_ns;
  if (!video_timebase.IsValid() || video_pts_ns != audio_pts_ns ||
      video_pts_ns != 2500000) {
    return Fail("Video and audio should normalize against one session origin.");
  }

  if (!olouie::record::BuildSessionClock(3, 3, &clock, &error) ||
      clock.origin_100ns != 10000000 || clock.origin_ns != 1000000000 ||
      olouie::record::BuildSessionClock(0, 1, &clock, &error) ||
      olouie::record::BuildSessionClock(1, 1, nullptr, &error)) {
    return Fail("Session clock scaling or invalid-input handling is wrong.");
  }

  if (!olouie::record::CaptureSessionClock(&clock, &error) ||
      !clock.IsValid()) {
    return Fail("The host should provide a valid monotonic session clock.");
  }
  return 0;
}

int TestPacketStore() {
  const auto root = RecordTestRoot(L"O'LouieRecordTests");
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
      olouie::record::TrackDefinition{2, olouie::record::CodecId::Aac},
  };

  std::wstring error;
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create failed: " << error << L'\n';
    return 1;
  }

  if (!Append(&store, VideoPacket(0, true), "v0") ||
      !Append(&store, AudioPacket(10 * kMs, 20 * kMs), "a10") ||
      !Append(&store, VideoPacket(33 * kMs, false), "v33") ||
      !Append(&store, AudioPacket(40 * kMs, 20 * kMs), "a40") ||
      !Append(&store, VideoPacket(66 * kMs, true), "v66") ||
      !Append(&store, AudioPacket(90 * kMs, 20 * kMs), "a90") ||
      !Append(&store, VideoPacket(99 * kMs, false), "v99")) {
    return 1;
  }

  const auto index = store.SnapshotIndex();
  const auto packet_stats = store.SnapshotStats();
  if (index.size() != 7 || packet_stats.packet_count != 7 ||
      packet_stats.payload_byte_count != 20 ||
      packet_stats.tracks.size() != 2 ||
      packet_stats.tracks[0].track_id != 1 ||
      packet_stats.tracks[0].codec_id != olouie::record::CodecId::H264 ||
      packet_stats.tracks[0].packet_count != 4 ||
      packet_stats.tracks[0].payload_byte_count != 11 ||
      packet_stats.tracks[1].track_id != 2 ||
      packet_stats.tracks[1].codec_id != olouie::record::CodecId::Aac ||
      packet_stats.tracks[1].packet_count != 3 ||
      packet_stats.tracks[1].payload_byte_count != 9) {
    return Fail("PacketStore index or lightweight statistics changed unexpectedly.");
  }

  const auto keyframe_range = store.QueryRange(80 * kMs, 120 * kMs, true);
  if (keyframe_range.actual_start_ns != 66 * kMs ||
      keyframe_range.packets.size() != 3) {
    return Fail("Keyframe-aligned PacketStore range is incorrect.");
  }

  std::vector<std::byte> payload;
  if (!store.ReadPayload(keyframe_range.packets.front(), &payload, &error) ||
      !PayloadEquals(payload, "v66")) {
    std::wcerr << L"Payload read failed: " << error << L'\n';
    return 1;
  }

  const auto exact_range = store.QueryRange(80 * kMs, 120 * kMs, false);
  if (exact_range.actual_start_ns != 80 * kMs ||
      exact_range.packets.size() != 3) {
    return Fail("Non-keyframe-aligned PacketStore range is incorrect.");
  }

  std::array<olouie::record::PacketIndexEntry, 2> straddling_packets{};
  straddling_packets[0].metadata = VideoPacket(0, false);
  straddling_packets[0].metadata.duration_ns = 40 * kMs;
  straddling_packets[1].metadata = VideoPacket(33 * kMs, true);
  const auto straddling_range = olouie::record::QueryPacketRange(
      straddling_packets, 35 * kMs, 60 * kMs, true);
  if (straddling_range.actual_start_ns != 33 * kMs ||
      straddling_range.packets.size() != 1 ||
      !straddling_range.packets.front().IsKeyframe()) {
    return Fail("Keyframe-aligned ranges should exclude an earlier video packet that straddles the selected keyframe.");
  }

  store.Close();

  {
    std::ofstream partial_tail(store.packet_file_path(),
                               std::ios::binary | std::ios::app);
    partial_tail << "partial";
  }

  auto recovered = olouie::record::PacketStore::Recover(session_dir, &error);
  const auto recovered_index = recovered.SnapshotIndex();
  const auto recovered_stats = recovered.SnapshotStats();
  if (recovered_index.size() != 7 ||
      recovered_stats.packet_count != packet_stats.packet_count ||
      recovered_stats.payload_byte_count != packet_stats.payload_byte_count ||
      recovered_stats.tracks.size() != packet_stats.tracks.size()) {
    std::wcerr << L"PacketStore recovery failed: " << error << L'\n';
    return 1;
  }

  const auto recovered_range =
      recovered.QueryRange(80 * kMs, 120 * kMs, true);
  if (recovered_range.actual_start_ns != 66 * kMs ||
      recovered_range.packets.size() != 3) {
    return Fail("Recovered PacketStore range is incorrect.");
  }

  if (!recovered.ReadPayload(recovered_range.packets.back(), &payload, &error) ||
      !PayloadEquals(payload, "v99")) {
    std::wcerr << L"Recovered payload read failed: " << error << L'\n';
    return 1;
  }

  olouie::record::MuxPlan mux_plan;
  const olouie::record::MuxPlanOptions mux_options;
  if (!olouie::record::BuildMuxPlan(recovered_range, tracks, mux_options,
                                    &mux_plan, &error)) {
    std::wcerr << L"MuxPlan build failed: " << error << L'\n';
    return 1;
  }

  if (!mux_plan.HasVideoTrack() || mux_plan.tracks.size() != 2 ||
      mux_plan.packets.size() != 3 ||
      mux_plan.source_start_ns != recovered_range.actual_start_ns ||
      mux_plan.source_end_ns != recovered_range.actual_end_ns) {
    return Fail("MuxPlan basic shape is incorrect.");
  }

  if (mux_plan.packets[0].packet.metadata.track_id != 1 ||
      mux_plan.packets[0].output_dts_ns != 0 ||
      mux_plan.packets[1].packet.metadata.track_id != 2 ||
      mux_plan.packets[1].output_dts_ns != 24 * kMs ||
      mux_plan.packets[2].packet.metadata.track_id != 1 ||
      mux_plan.packets[2].output_dts_ns != 33 * kMs) {
    return Fail("MuxPlan packet ordering or timestamp normalization is incorrect.");
  }

  olouie::record::MuxPlanOptions passthrough_options;
  passthrough_options.normalize_timestamps = false;
  if (!olouie::record::BuildMuxPlan(recovered_range, tracks,
                                    passthrough_options, &mux_plan, &error) ||
      mux_plan.packets[0].output_dts_ns != 66 * kMs) {
    return Fail("MuxPlan timestamp passthrough is incorrect.");
  }

  const std::array audio_only_track{
      olouie::record::TrackDefinition{2, olouie::record::CodecId::Aac},
  };
  olouie::record::PacketRange audio_only_range;
  audio_only_range.actual_start_ns = 0;
  audio_only_range.actual_end_ns = 100 * kMs;
  audio_only_range.packets.push_back(recovered_index[1]);

  if (olouie::record::BuildMuxPlan(audio_only_range, audio_only_track,
                                   mux_options, &mux_plan, &error)) {
    return Fail("MuxPlan should reject audio-only plans by default.");
  }

  olouie::record::MuxPlanOptions audio_allowed_options;
  audio_allowed_options.require_video_track = false;
  if (!olouie::record::BuildMuxPlan(audio_only_range, audio_only_track,
                                    audio_allowed_options, &mux_plan,
                                    &error) ||
      mux_plan.HasVideoTrack() || mux_plan.tracks.size() != 1) {
    return Fail("MuxPlan audio-only override is incorrect.");
  }

  olouie::record::MuxPlan mp4_plan;
  if (!olouie::record::BuildMuxPlan(recovered_range, tracks, mux_options,
                                    &mp4_plan, &error)) {
    std::wcerr << L"MP4 test MuxPlan build failed: " << error << L'\n';
    return 1;
  }

  olouie::record::Mp4MuxRequest mux_request;
  mux_request.temp_output_path = root / L"exports" / L"clip.tmp";
  mux_request.final_output_path = root / L"exports" / L"clip.mp4";
  mux_request.packet_file_path = store.packet_file_path();
  mux_request.video_track = MakeReadyMp4VideoTrack(1);
  mux_request.audio_tracks = {MakeReadyMp4AudioTrack(2)};
  mux_request.plan = mp4_plan;

  const auto validation =
      olouie::record::Mp4Muxer::ValidateRequest(mux_request);
  if (!validation.Succeeded()) {
    std::wcerr << L"MP4 mux validation failed: " << validation.message << L'\n';
    return 1;
  }

  olouie::record::Mp4MuxPayloadReadStats read_stats;
  const auto read_dry_run =
      olouie::record::Mp4Muxer::DryRunPayloadRead(mux_request, &read_stats);
  if (!read_dry_run.Succeeded() || read_stats.packet_count != 3 ||
      read_stats.video_packet_count != 2 ||
      read_stats.audio_packet_count != 1 ||
      read_stats.payload_byte_count != 9 ||
      read_stats.video_payload_byte_count != 6 ||
      read_stats.audio_payload_byte_count != 3 ||
      read_stats.first_video_dts_ns != 0 ||
      read_stats.last_video_dts_ns != 33 * kMs) {
    std::wcerr << L"MP4 payload dry run failed: " << read_dry_run.message
               << L'\n';
    return 1;
  }

  const auto backend = olouie::record::Mp4Muxer::BackendAvailability();
  if (backend.required_libraries.size() != 4 ||
      backend.required_libraries[0] != L"avformat" ||
      !backend.dynamic_linking_expected ||
      (backend.status != olouie::record::Mp4MuxBackendStatus::NotConfigured &&
       backend.status != olouie::record::Mp4MuxBackendStatus::Configured) ||
      backend.Available() !=
          (backend.status == olouie::record::Mp4MuxBackendStatus::Configured) ||
      backend.message.empty()) {
    return Fail("MP4 backend availability shape is incorrect.");
  }

  if (std::wstring(olouie::record::Mp4MuxBackendStatusName(
          olouie::record::Mp4MuxBackendStatus::NotConfigured)) !=
      L"not configured") {
    return Fail("MP4 backend status names changed unexpectedly.");
  }

  olouie::record::Mp4MuxStreamSetupStats stream_setup_stats;
  const auto stream_setup =
      olouie::record::Mp4Muxer::ValidateVideoStreamSetup(
          mux_request, &stream_setup_stats);
  if (backend.Available()) {
    if (!stream_setup.Succeeded() || !stream_setup_stats.IsReady() ||
        stream_setup_stats.video_track_id !=
            mux_request.video_track.track_id ||
        stream_setup_stats.width != mux_request.video_track.width ||
        stream_setup_stats.height != mux_request.video_track.height ||
        stream_setup_stats.fps_numerator !=
            mux_request.video_track.fps_numerator ||
        stream_setup_stats.fps_denominator !=
            mux_request.video_track.fps_denominator ||
        stream_setup_stats.extradata_bytes !=
            mux_request.video_track.avcc_extradata.size() ||
        stream_setup_stats.audio_stream_count != 1 ||
        stream_setup_stats.aac_parameters_applied_count != 1 ||
        stream_setup_stats.audio_extradata_bytes != 2) {
      std::wcerr << L"MP4 stream setup validation failed: "
                 << stream_setup.message << L'\n';
      return 1;
    }
  } else if (stream_setup.status !=
                 olouie::record::Mp4MuxStatus::BackendUnavailable ||
             stream_setup_stats.IsReady()) {
    return Fail("MP4 stream setup should report unavailable backend by default.");
  }

  if (olouie::record::Mp4Muxer::ValidateVideoStreamSetup(mux_request, nullptr)
          .status != olouie::record::Mp4MuxStatus::InvalidRequest) {
    return Fail("MP4 stream setup should require a stats destination.");
  }

  const olouie::record::Mp4Muxer muxer;
  const auto write_result = muxer.WriteMp4(mux_request);
  if (backend.Available()) {
    if (write_result.status != olouie::record::Mp4MuxStatus::InvalidRequest ||
        std::filesystem::exists(mux_request.temp_output_path)) {
      return Fail("MP4 writer should reject non-Annex-B payloads and clean its partial file.");
    }
  } else if (write_result.status !=
             olouie::record::Mp4MuxStatus::BackendUnavailable) {
    return Fail("MP4 muxer skeleton should report backend unavailable.");
  }

  {
    const auto h264_session_dir = root / L"h264-session";
    const std::array h264_tracks{
        olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
    };
    auto h264_store =
        olouie::record::PacketStore::Create(h264_session_dir, h264_tracks,
                                            &error);
    if (!h264_store.IsWritable()) {
      std::wcerr << L"H.264 PacketStore create failed: " << error << L'\n';
      return 1;
    }

    if (!Append(&h264_store, VideoPacket(0, true),
                {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
                 0x00, 0x00, 0x01,       0x68, 0xce, 0x06,
                 0x00, 0x00, 0x01,       0x65, 0x88, 0x84}) ||
        !Append(&h264_store, VideoPacket(33 * kMs, false),
                {0x00, 0x00, 0x01, 0x41, 0x9a, 0x22})) {
      return 1;
    }
    h264_store.Close();

    auto h264_recovered =
        olouie::record::PacketStore::Recover(h264_session_dir, &error);
    auto h264_range = h264_recovered.QueryRange(0, 70 * kMs, true);
    olouie::record::MuxPlan h264_plan;
    if (!olouie::record::BuildMuxPlan(h264_range, h264_tracks, mux_options,
                                      &h264_plan, &error)) {
      std::wcerr << L"H.264 MP4 MuxPlan build failed: " << error << L'\n';
      return 1;
    }

    olouie::record::Mp4MuxRequest h264_request;
    h264_request.temp_output_path = root / L"exports" / L"h264.tmp";
    h264_request.final_output_path = root / L"exports" / L"h264.mp4";
    h264_request.packet_file_path = h264_recovered.packet_file_path();
    h264_request.video_track = MakeReadyMp4VideoTrack(1);
    h264_request.plan = h264_plan;

    const auto h264_write = muxer.WriteMp4(h264_request);
    if (backend.Available()) {
      if (!h264_write.Succeeded() ||
          !std::filesystem::exists(h264_request.final_output_path) ||
          std::filesystem::exists(h264_request.temp_output_path)) {
        std::wcerr << L"H.264 MP4 write failed: " << h264_write.message
                   << L'\n';
        return 1;
      }
    } else if (h264_write.status !=
                   olouie::record::Mp4MuxStatus::BackendUnavailable ||
               std::filesystem::exists(h264_request.final_output_path) ||
               std::filesystem::exists(h264_request.temp_output_path)) {
      return Fail("H.264 MP4 write should stay unavailable without FFmpeg.");
    }

    if (backend.Available()) {
      const auto preserved_manifest =
          MakeReadyVideoManifest(h264_session_dir, 1);
      if (!olouie::record::WriteSessionManifest(preserved_manifest)
               .Succeeded()) {
        return Fail("Stale-partial recovery fixture manifest failed.");
      }

      auto stale_request = h264_request;
      stale_request.temp_output_path =
          root / L"exports" / L"stale.partial.mp4";
      stale_request.final_output_path =
          root / L"exports" / L"stale-output.mp4";
      std::filesystem::create_directory(stale_request.temp_output_path);
      {
        std::ofstream marker(stale_request.temp_output_path / L"keep.txt",
                             std::ios::binary | std::ios::trunc);
        marker << "nonempty stale partial";
      }
      const auto stale_failure = muxer.WriteMp4(stale_request);
      olouie::record::SessionManifest read_back_manifest;
      if (stale_failure.status !=
              olouie::record::Mp4MuxStatus::FileSystemError ||
          stale_failure.write_fault.operation !=
              olouie::record::DiskWriteOperation::RemoveStalePartial ||
          stale_failure.write_fault.path != stale_request.temp_output_path ||
          std::filesystem::exists(stale_request.final_output_path) ||
          !std::filesystem::exists(stale_request.temp_output_path /
                                   L"keep.txt") ||
          h264_recovered.SnapshotIndex().size() != 2 ||
          !olouie::record::ReadSessionManifest(h264_session_dir,
                                               &read_back_manifest)
               .Succeeded()) {
        return Fail("Stale MP4 partial failure should preserve the session and report cleanup precisely.");
      }
      std::filesystem::remove_all(stale_request.temp_output_path);

      auto publish_request = h264_request;
      publish_request.temp_output_path =
          root / L"exports" / L"publish-failure.partial.mp4";
      publish_request.final_output_path =
          root / L"exports" / L"publish-failure.mp4";
      publish_request.allow_overwrite = true;
      std::filesystem::create_directory(publish_request.final_output_path);
      {
        std::ofstream marker(publish_request.final_output_path / L"keep.txt",
                             std::ios::binary | std::ios::trunc);
        marker << "existing destination remains";
      }
      const auto publish_failure = muxer.WriteMp4(publish_request);
      std::error_code size_error;
      const auto preserved_temp_size = std::filesystem::file_size(
          publish_request.temp_output_path, size_error);
      if (publish_failure.status !=
              olouie::record::Mp4MuxStatus::FileSystemError ||
          publish_failure.write_fault.operation !=
              olouie::record::DiskWriteOperation::AtomicPublish ||
          publish_failure.write_fault.path !=
              publish_request.final_output_path ||
          publish_failure.message.find(L"complete temporary MP4") ==
              std::wstring::npos ||
          size_error || preserved_temp_size == 0 ||
          !std::filesystem::exists(publish_request.final_output_path /
                                   L"keep.txt")) {
        return Fail("MP4 publication failure should preserve the complete temp and existing destination.");
      }
      std::filesystem::remove(publish_request.temp_output_path);
      std::filesystem::remove_all(publish_request.final_output_path);
    }

    auto silent_audio_manifest = MakeReadyVideoManifest(h264_session_dir, 1);
    silent_audio_manifest.audio_tracks.push_back(
        MakeReadyAudioManifestTrack(2));
    const auto silent_manifest_write =
        olouie::record::WriteSessionManifest(silent_audio_manifest);
    olouie::record::VideoExportPlanOptions silent_export_options;
    silent_export_options.requested_start_ns = 0;
    silent_export_options.requested_end_ns = 70 * kMs;
    olouie::record::VideoExportPlan silent_export_plan;
    const auto silent_export =
        olouie::record::BuildRecoveredVideoExportPlan(
            h264_session_dir, silent_export_options, &silent_export_plan);
    olouie::record::Mp4MuxRequest silent_request;
    const auto silent_request_build =
        olouie::record::BuildVideoMp4MuxRequest(
            silent_export_plan,
            root / L"exports" / L"h264-silent-audio.tmp",
            root / L"exports" / L"h264-silent-audio.mp4", false,
            &silent_request);
    if (!silent_manifest_write.Succeeded() || !silent_export.Succeeded() ||
        !silent_export_plan.IsReady() ||
        !silent_export_plan.audio_tracks.empty() ||
        silent_export_plan.omitted_audio_track_ids !=
            std::vector<uint32_t>({2}) ||
        silent_export_plan.mux_plan.tracks.size() != 1 ||
        !silent_request_build.Succeeded() ||
        !silent_request.audio_tracks.empty()) {
      std::wcerr << L"Silent-audio H.264 export planning failed: "
                 << silent_export.message << L' ' << silent_request_build.message
                 << L'\n';
      return 1;
    }

    const auto silent_write = muxer.WriteMp4(silent_request);
    if (backend.Available()) {
      if (!silent_write.Succeeded() ||
          !std::filesystem::exists(silent_request.final_output_path) ||
          std::filesystem::exists(silent_request.temp_output_path)) {
        std::wcerr << L"Silent-audio video-only MP4 write failed: "
                   << silent_write.message << L'\n';
        return 1;
      }
    } else if (silent_write.status !=
                   olouie::record::Mp4MuxStatus::BackendUnavailable ||
               std::filesystem::exists(silent_request.final_output_path) ||
               std::filesystem::exists(silent_request.temp_output_path)) {
      return Fail("Silent-audio MP4 write should stay unavailable without FFmpeg.");
    }
  }

  {
    const auto mixed_session_dir = root / L"h264-aac-session";
    const std::array mixed_tracks{
        olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
        olouie::record::TrackDefinition{2, olouie::record::CodecId::Aac},
    };
    auto mixed_store = olouie::record::PacketStore::Create(
        mixed_session_dir, mixed_tracks, &error);
    if (!mixed_store.IsWritable() ||
        !Append(&mixed_store, VideoPacket(0, true),
                {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
                 0x00, 0x00, 0x01,       0x68, 0xce, 0x06,
                 0x00, 0x00, 0x01,       0x65, 0x88, 0x84}) ||
        !Append(&mixed_store, AudioPacket(0, 21333333),
                {0x21, 0x10, 0x04, 0x60}) ||
        !Append(&mixed_store, AudioPacket(21333333, 21333333),
                {0x21, 0x10, 0x04, 0x60}) ||
        !Append(&mixed_store, VideoPacket(33 * kMs, false),
                {0x00, 0x00, 0x01, 0x41, 0x9a, 0x22})) {
      return Fail("Mixed H.264/AAC PacketStore fixture failed.");
    }
    mixed_store.Close();

    auto mixed_recovered =
        olouie::record::PacketStore::Recover(mixed_session_dir, &error);
    auto mixed_range = mixed_recovered.QueryRange(0, 70 * kMs, true);
    olouie::record::MuxPlan mixed_plan;
    if (!olouie::record::BuildMuxPlan(mixed_range, mixed_tracks, mux_options,
                                      &mixed_plan, &error)) {
      std::wcerr << L"Mixed MP4 MuxPlan build failed: " << error << L'\n';
      return 1;
    }

    olouie::record::Mp4MuxRequest mixed_request;
    mixed_request.temp_output_path = root / L"exports" / L"h264-aac.tmp";
    mixed_request.final_output_path = root / L"exports" / L"h264-aac.mp4";
    mixed_request.packet_file_path = mixed_recovered.packet_file_path();
    mixed_request.video_track = MakeReadyMp4VideoTrack(1);
    mixed_request.audio_tracks = {MakeReadyMp4AudioTrack(2)};
    mixed_request.plan = std::move(mixed_plan);

    const auto mixed_validation =
        olouie::record::Mp4Muxer::ValidateRequest(mixed_request);
    if (!mixed_validation.Succeeded()) {
      std::wcerr << L"Mixed MP4 validation failed: "
                 << mixed_validation.message << L'\n';
      return 1;
    }

    const auto mixed_write = muxer.WriteMp4(mixed_request);
    if (backend.Available()) {
      if (!mixed_write.Succeeded() ||
          !std::filesystem::exists(mixed_request.final_output_path) ||
          std::filesystem::exists(mixed_request.temp_output_path)) {
        std::wcerr << L"Mixed H.264/AAC MP4 write failed: "
                   << mixed_write.message << L'\n';
        return 1;
      }
    } else if (mixed_write.status !=
                   olouie::record::Mp4MuxStatus::BackendUnavailable ||
               std::filesystem::exists(mixed_request.final_output_path) ||
               std::filesystem::exists(mixed_request.temp_output_path)) {
      return Fail("Mixed MP4 write should stay unavailable without FFmpeg.");
    }
  }

  auto invalid_request = mux_request;
  invalid_request.final_output_path = root / L"exports" / L"clip.mkv";
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject non-mp4 final output paths.");
  }

  invalid_request = mux_request;
  invalid_request.video_track.avcc_extradata.clear();
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject missing H.264 extradata.");
  }

  invalid_request = mux_request;
  invalid_request.video_track.track_id = 7;
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject mismatched video metadata tracks.");
  }

  invalid_request = mux_request;
  invalid_request.audio_tracks[0].audio_specific_config.clear();
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject missing AAC decoder configuration.");
  }

  invalid_request = mux_request;
  invalid_request.audio_tracks.clear();
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject AAC plan tracks without metadata.");
  }

  invalid_request = mux_request;
  invalid_request.packet_file_path.clear();
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject missing packet file paths.");
  }

  invalid_request = mux_request;
  invalid_request.plan.packets.front().packet.metadata.flags =
      olouie::record::PacketFlagNone;
  if (olouie::record::Mp4Muxer::ValidateRequest(invalid_request).Succeeded()) {
    return Fail("MP4 muxer should reject non-keyframe video starts.");
  }

  invalid_request = mux_request;
  invalid_request.packet_file_path = root / L"missing.dat";
  if (olouie::record::Mp4Muxer::DryRunPayloadRead(invalid_request,
                                                  &read_stats)
          .status != olouie::record::Mp4MuxStatus::FileSystemError) {
    return Fail("MP4 payload dry run should reject missing packet files.");
  }

  invalid_request = mux_request;
  invalid_request.plan.packets.back().output_dts_ns = -1;
  if (olouie::record::Mp4Muxer::DryRunPayloadRead(invalid_request,
                                                  &read_stats)
          .status != olouie::record::Mp4MuxStatus::InvalidRequest) {
    return Fail("MP4 payload dry run should reject non-monotonic video DTS.");
  }

  if (std::wstring(olouie::record::Mp4MuxStatusName(
          olouie::record::Mp4MuxStatus::BackendUnavailable)) !=
      L"backend unavailable") {
    return Fail("MP4 mux status names changed unexpectedly.");
  }

  std::filesystem::create_directories(root / L"exports");
  {
    std::ofstream temp_file(mux_request.temp_output_path,
                            std::ios::binary | std::ios::trunc);
    temp_file << "pending mp4";
  }

  const auto rename_result = olouie::record::Mp4Muxer::AtomicRename(
      mux_request.temp_output_path, mux_request.final_output_path, false);
  if (!rename_result.Succeeded() ||
      !std::filesystem::exists(mux_request.final_output_path) ||
      std::filesystem::exists(mux_request.temp_output_path)) {
    std::wcerr << L"Atomic rename failed: " << rename_result.message << L'\n';
    return 1;
  }

  const auto second_temp = root / L"exports" / L"second.tmp";
  {
    std::ofstream temp_file(second_temp, std::ios::binary | std::ios::trunc);
    temp_file << "second";
  }

  const auto existing_result = olouie::record::Mp4Muxer::AtomicRename(
      second_temp, mux_request.final_output_path, false);
  if (existing_result.status !=
          olouie::record::Mp4MuxStatus::DestinationExists ||
      !std::filesystem::exists(second_temp)) {
    return Fail("Atomic rename should preserve temp file when destination exists.");
  }

  const auto overwrite_result = olouie::record::Mp4Muxer::AtomicRename(
      second_temp, mux_request.final_output_path, true);
  if (!overwrite_result.Succeeded() || std::filesystem::exists(second_temp)) {
    return Fail("Atomic rename overwrite behavior is incorrect.");
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestAsynchronousPacketWriter() {
  const auto root = RecordTestRoot(L"O'LouieAsyncPacketWriterTests");
  std::filesystem::remove_all(root);
  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264}};

  std::wstring error;
  auto store = olouie::record::PacketStore::Create(
      root / L"drain", tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"Async PacketStore create failed: " << error << L'\n';
    return 1;
  }

  for (int64_t index = 0; index < 64; ++index) {
    if (!Append(&store, VideoPacket(index * 33 * kMs, index == 0),
                "async-packet-payload")) {
      return 1;
    }
  }
  const auto accepted_stats = store.SnapshotStats();
  if (accepted_stats.packet_count != 64) {
    return Fail("Async PacketStore statistics should count accepted packets immediately.");
  }

  olouie::record::PacketStoreExportSnapshot export_snapshot;
  if (!store.SnapshotForExport(&export_snapshot, &error) ||
      export_snapshot.index.size() != 64) {
    std::wcerr << L"Async export barrier failed: " << error << L'\n';
    return 1;
  }
  auto writer_stats = store.SnapshotWriterStats();
  if (writer_stats.enqueued_packet_count != 64 ||
      writer_stats.persisted_packet_count != 64 ||
      writer_stats.queued_packet_count != 0 ||
      writer_stats.peak_queued_packet_count == 0 ||
      writer_stats.flush_count == 0) {
    return Fail("Async packet writer telemetry or export flush accounting is incorrect.");
  }

  for (int64_t index = 64; index < 128; ++index) {
    if (!Append(&store, VideoPacket(index * 33 * kMs, false),
                "close-drain-payload")) {
      return 1;
    }
  }
  if (!store.Close(&error)) {
    std::wcerr << L"Async PacketStore close/drain failed: " << error << L'\n';
    return 1;
  }
  writer_stats = store.SnapshotWriterStats();
  auto recovered = olouie::record::PacketStore::Recover(
      root / L"drain", &error);
  if (writer_stats.persisted_packet_count != 128 ||
      recovered.SnapshotIndex().size() != 128) {
    return Fail("Closing PacketStore should drain every accepted queued packet before recovery.");
  }

  olouie::record::PacketStoreWriterOptions bounded_options;
  bounded_options.max_queued_packet_count = 1;
  bounded_options.max_queued_payload_bytes = 4;
  auto bounded = olouie::record::PacketStore::Create(
      root / L"bounded", tracks, &error, nullptr, bounded_options);
  auto oversized_payload = Bytes("12345");
  error.clear();
  if (!bounded.IsWritable() ||
      bounded.AppendPacket(VideoPacket(0, true), oversized_payload, &error) ||
      !bounded.last_write_fault().Failed() || bounded.IsWritable() ||
      bounded.SnapshotWriterStats().rejected_packet_count != 1 ||
      error.find(L"bounded packet writer queue") == std::wstring::npos) {
    return Fail("A full bounded packet queue should latch an actionable write fault without dropping silently.");
  }
  if (bounded.Close(&error)) {
    return Fail("Closing a PacketStore with a latched queue overflow should report failure.");
  }

  olouie::record::PacketStoreWriterOptions invalid_options;
  invalid_options.max_queued_packet_count = 0;
  error.clear();
  auto invalid = olouie::record::PacketStore::Create(
      root / L"invalid", tracks, &error, nullptr, invalid_options);
  if (invalid.IsWritable() ||
      error.find(L"queue limits") == std::wstring::npos) {
    return Fail("PacketStore should reject invalid asynchronous writer limits.");
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestDiskWriteFailures() {
  using olouie::record::DiskWriteFaultKind;
  using olouie::record::DiskWriteOperation;
  using olouie::record::DiskWriteSubsystem;

  const auto root = RecordTestRoot(L"O'LouieDiskWriteFailureTests");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto no_space = olouie::record::MakeDiskWriteFault(
      DiskWriteSubsystem::PacketStore, DiskWriteOperation::Append,
      root / L"session" / L"packets.dat",
      std::make_error_code(std::errc::no_space_on_device));
  const auto no_space_message =
      olouie::record::DescribeDiskWriteFault(no_space);
  if (no_space.kind != DiskWriteFaultKind::NoSpace ||
      no_space_message.find(L"PacketStore") == std::wstring::npos ||
      no_space_message.find(L"append") == std::wstring::npos ||
      no_space_message.find(L"packets.dat") == std::wstring::npos ||
      no_space_message.find(L"out of disk space") == std::wstring::npos) {
    return Fail("Simulated disk-full diagnostics should name the subsystem, operation, and path.");
  }

  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
  };
  const auto blocking_file = root / L"not-a-directory";
  {
    std::ofstream output(blocking_file, std::ios::binary | std::ios::trunc);
    output << "block";
  }
  std::wstring error;
  olouie::record::DiskWriteFault create_fault;
  auto failed_store = olouie::record::PacketStore::Create(
      blocking_file / L"session", tracks, &error, &create_fault);
  if (failed_store.IsWritable() || !create_fault.Failed() ||
      create_fault.subsystem != DiskWriteSubsystem::PacketStore ||
      create_fault.operation != DiskWriteOperation::CreateDirectories ||
      error.find(L"PacketStore") == std::wstring::npos ||
      error.find(L"not-a-directory") == std::wstring::npos) {
    return Fail("PacketStore create failures should retain typed path diagnostics.");
  }

  const auto session_dir = root / L"recoverable-session";
  auto store =
      olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable() ||
      !Append(&store, VideoPacket(0, true), "recoverable-video") ||
      !store.Close(&error)) {
    std::wcerr << L"Recoverable PacketStore fixture failed: " << error
               << L'\n';
    return 1;
  }

  auto manifest = MakeReadyVideoManifest(session_dir, 1);
  const auto initial_write = olouie::record::WriteSessionManifest(manifest);
  if (!initial_write.Succeeded()) {
    std::wcerr << L"Initial manifest write failed: " << initial_write.message
               << L'\n';
    return 1;
  }
  const auto manifest_path = olouie::record::SessionManifestPath(session_dir);
  std::ifstream initial_input(manifest_path, std::ios::binary);
  const std::string initial_manifest(
      (std::istreambuf_iterator<char>(initial_input)),
      std::istreambuf_iterator<char>());
  initial_input.close();

  auto manifest_temp_path = manifest_path;
  manifest_temp_path += L".tmp";
  std::filesystem::create_directory(manifest_temp_path);
  manifest.video.encoder_name = L"This update must not replace the old file";
  const auto failed_manifest =
      olouie::record::WriteSessionManifest(manifest);
  std::ifstream preserved_input(manifest_path, std::ios::binary);
  const std::string preserved_manifest(
      (std::istreambuf_iterator<char>(preserved_input)),
      std::istreambuf_iterator<char>());
  preserved_input.close();
  if (failed_manifest.status !=
          olouie::record::SessionManifestStatus::WriteFailed ||
      !failed_manifest.write_fault.Failed() ||
      failed_manifest.write_fault.subsystem !=
          DiskWriteSubsystem::SessionManifest ||
      failed_manifest.write_fault.operation !=
          DiskWriteOperation::OpenTemporaryFile ||
      failed_manifest.message.find(L"session.json.tmp") ==
          std::wstring::npos ||
      preserved_manifest != initial_manifest) {
    return Fail("Manifest write failure should preserve the published metadata and report its temp path.");
  }
  std::filesystem::remove_all(manifest_temp_path);

  olouie::record::SessionManifest recovered_manifest;
  auto recovered_store =
      olouie::record::PacketStore::Recover(session_dir, &error);
  if (!olouie::record::ReadSessionManifest(session_dir, &recovered_manifest)
           .Succeeded() ||
      recovered_store.SnapshotIndex().size() != 1 ||
      recovered_manifest.video.encoder_name == manifest.video.encoder_name) {
    return Fail("Published metadata and PacketStore data should remain recoverable after a manifest failure.");
  }

  const auto manifest_publish_dir = root / L"manifest-publish-failure";
  std::filesystem::create_directories(manifest_publish_dir);
  const auto manifest_publish_path =
      olouie::record::SessionManifestPath(manifest_publish_dir);
  std::filesystem::create_directory(manifest_publish_path);
  {
    std::ofstream marker(manifest_publish_path / L"keep.txt",
                         std::ios::binary | std::ios::trunc);
    marker << "existing manifest destination remains";
  }
  const auto manifest_publish_failure =
      olouie::record::WriteSessionManifest(
          MakeReadyVideoManifest(manifest_publish_dir, 1));
  auto manifest_publish_temp = manifest_publish_path;
  manifest_publish_temp += L".tmp";
  std::error_code manifest_size_error;
  const auto manifest_temp_size = std::filesystem::file_size(
      manifest_publish_temp, manifest_size_error);
  if (manifest_publish_failure.status !=
          olouie::record::SessionManifestStatus::WriteFailed ||
      manifest_publish_failure.write_fault.operation !=
          DiskWriteOperation::AtomicPublish ||
      manifest_publish_failure.write_fault.path != manifest_publish_path ||
      manifest_publish_failure.message.find(L"complete temporary manifest") ==
          std::wstring::npos ||
      manifest_size_error || manifest_temp_size == 0 ||
      !std::filesystem::exists(manifest_publish_path / L"keep.txt")) {
    return Fail("Manifest publication failure should preserve its complete temp and existing destination.");
  }
  std::filesystem::remove(manifest_publish_temp);
  std::filesystem::remove_all(manifest_publish_path);

  const auto publish_temp = root / L"publish.tmp";
  const auto publish_destination = root / L"existing-output.mp4";
  {
    std::ofstream output(publish_temp, std::ios::binary | std::ios::trunc);
    output << "new output";
  }
  std::filesystem::create_directory(publish_destination);
  {
    std::ofstream marker(publish_destination / L"keep.txt",
                         std::ios::binary | std::ios::trunc);
    marker << "old output remains";
  }
  const auto publish_failure = olouie::record::Mp4Muxer::AtomicRename(
      publish_temp, publish_destination, true);
  if (publish_failure.status !=
          olouie::record::Mp4MuxStatus::FileSystemError ||
      publish_failure.write_fault.operation !=
          DiskWriteOperation::AtomicPublish ||
      publish_failure.write_fault.path != publish_destination ||
      !std::filesystem::exists(publish_temp) ||
      !std::filesystem::exists(publish_destination / L"keep.txt")) {
    return Fail("Failed atomic publication should preserve both the old destination and new temp file.");
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestBookmarks() {
  olouie::record::BookmarkCollection bookmarks;
  std::wstring error;

  olouie::record::Bookmark first;
  if (!bookmarks.Add(90 * kMs, 30 * kMs, 10 * kMs, L"", L"round start",
                     &first, &error)) {
    std::wcerr << L"Bookmark add failed: " << error << L'\n';
    return 1;
  }

  if (first.id != 1 || first.label != L"Bookmark 1" ||
      first.time_ns != 90 * kMs || first.user_note != L"round start") {
    return Fail("Default bookmark fields are incorrect.");
  }

  olouie::record::Bookmark second;
  if (!bookmarks.Add(5 * kMs, 60 * kMs, 0, L"Opening", L"", &second,
                     &error)) {
    std::wcerr << L"Second bookmark add failed: " << error << L'\n';
    return 1;
  }

  if (bookmarks.Count() != 2 || bookmarks.Find(2) == nullptr ||
      bookmarks.Find(3) != nullptr) {
    return Fail("Bookmark lookup/count is incorrect.");
  }

  olouie::record::BookmarkExportRange range;
  if (!olouie::record::ResolveBookmarkExportRange(first, 0, &range, &error) ||
      range.bookmark_id != 1 || range.actual_start_ns != 60 * kMs ||
      range.actual_end_ns != 100 * kMs || range.clamped_to_session_start) {
    std::wcerr << L"Bookmark range resolution failed: " << error << L'\n';
    return 1;
  }

  if (!olouie::record::ResolveBookmarkExportRange(
          second, 0, &range, &error) ||
      range.requested_start_ns != -55 * kMs || range.actual_start_ns != 0 ||
      range.actual_end_ns != 5 * kMs || !range.clamped_to_session_start) {
    return Fail("Bookmark session-start clamping is incorrect.");
  }

  if (!olouie::record::ResolveBookmarkExportRange(
          first, 0, &range, &error, 20 * kMs, 15 * kMs) ||
      range.actual_start_ns != 70 * kMs ||
      range.actual_end_ns != 105 * kMs) {
    return Fail("Bookmark custom export durations are incorrect.");
  }

  const auto snapshot = bookmarks.Snapshot();
  if (snapshot.size() != 2 || snapshot[1].label != L"Opening") {
    return Fail("Bookmark snapshot is incorrect.");
  }

  olouie::record::Bookmark ignored;
  if (bookmarks.Add(10 * kMs, 0, 0, L"Invalid", L"", &ignored, &error) ||
      bookmarks.Count() != 2) {
    return Fail("Invalid bookmark duration should be rejected.");
  }

  if (olouie::record::ResolveBookmarkExportRange(
          first, 0, &range, &error, -1, 10 * kMs)) {
    return Fail("Invalid bookmark export override should be rejected.");
  }

  return 0;
}

int TestSessionManifest() {
  const auto root = RecordTestRoot(L"O'LouieSessionManifestTests");
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  auto manifest = MakeReadyVideoManifest(session_dir, 7);
  manifest.audio_tracks.push_back(MakeReadyAudioManifestTrack(8));
  manifest.bookmarks.push_back(
      {1, 90 * kMs, L"Round start", 60 * kMs, 0, L"first note"});
  manifest.bookmarks.push_back(
      {2, 120 * kMs, L"Bookmark 2", 30 * kMs, 10 * kMs, L""});

  if (!manifest.IsReady()) {
    return Fail("Session manifest test fixture should be ready.");
  }

  olouie::record::SessionManifest invalid_manifest;
  if (olouie::record::WriteSessionManifest(invalid_manifest).status !=
      olouie::record::SessionManifestStatus::InvalidConfig) {
    return Fail("Session manifest write should reject incomplete metadata.");
  }

  const auto write = olouie::record::WriteSessionManifest(manifest);
  if (!write.Succeeded() ||
      !std::filesystem::exists(
          olouie::record::SessionManifestPath(session_dir))) {
    std::wcerr << L"Session manifest write failed: " << write.message << L'\n';
    return 1;
  }

  olouie::record::SessionManifest recovered;
  const auto read = olouie::record::ReadSessionManifest(session_dir, &recovered);
  if (!read.Succeeded() || !recovered.IsReady() ||
      recovered.video.track_id != 7 ||
      recovered.video.h264_avcc_extradata !=
          manifest.video.h264_avcc_extradata ||
      recovered.video.encoder_name != manifest.video.encoder_name ||
      recovered.video.encoder_clsid != L"{fake-clsid}" ||
      !recovered.video.d3d11_aware ||
      recovered.video.media_gop_frame_count != 120 ||
      recovered.video.monitor_device_name != L"DISPLAY1" ||
      recovered.video.source_width != 1920 ||
      recovered.video.output_width != 1920 ||
      recovered.audio_tracks.size() != 1 ||
      recovered.audio_tracks[0].track_id != 8 ||
      recovered.audio_tracks[0].source_kind != L"system_loopback" ||
      recovered.audio_tracks[0].sample_rate != 48000 ||
      recovered.audio_tracks[0].channel_count != 2 ||
      recovered.audio_tracks[0].aac_audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x90}) ||
      recovered.audio_tracks[0].encoder_name != L"Fake AAC Encoder" ||
      recovered.bookmarks.size() != 2 ||
      recovered.bookmarks[0].id != 1 ||
      recovered.bookmarks[0].time_ns != 90 * kMs ||
      recovered.bookmarks[0].label != L"Round start" ||
      recovered.bookmarks[0].default_pre_ns != 60 * kMs ||
      recovered.bookmarks[0].user_note != L"first note" ||
      recovered.bookmarks[1].default_post_ns != 10 * kMs) {
    std::wcerr << L"Session manifest read failed: " << read.message << L'\n';
    return 1;
  }

  {
    std::ofstream packets(session_dir / L"packets.dat", std::ios::binary);
    packets.put('\0');
  }
  const auto relocated_session_dir = root / L"relocated-session";
  std::filesystem::rename(session_dir, relocated_session_dir);
  const auto relocated_read = olouie::record::ReadSessionManifest(
      relocated_session_dir, &recovered);
  if (!relocated_read.Succeeded() ||
      recovered.session_dir != relocated_session_dir ||
      recovered.packet_file_path != relocated_session_dir / L"packets.dat") {
    return Fail("Relocated session manifests should use their actual paths.");
  }
  std::filesystem::rename(relocated_session_dir, session_dir);

  if (olouie::record::ReadSessionManifest(root / L"missing", &recovered)
          .status != olouie::record::SessionManifestStatus::ReadFailed) {
    return Fail("Session manifest read should report missing files.");
  }

  auto duplicate_tracks = manifest;
  duplicate_tracks.audio_tracks[0].track_id = duplicate_tracks.video.track_id;
  if (duplicate_tracks.IsReady() ||
      olouie::record::WriteSessionManifest(duplicate_tracks).status !=
          olouie::record::SessionManifestStatus::InvalidConfig) {
    return Fail("Session manifest should reject duplicate track ids.");
  }

  auto duplicate_bookmarks = manifest;
  duplicate_bookmarks.bookmarks[1].id =
      duplicate_bookmarks.bookmarks[0].id;
  if (duplicate_bookmarks.IsReady() ||
      olouie::record::WriteSessionManifest(duplicate_bookmarks).status !=
          olouie::record::SessionManifestStatus::InvalidConfig) {
    return Fail("Session manifest should reject duplicate bookmark ids.");
  }

  {
    const auto path = olouie::record::SessionManifestPath(session_dir);
    std::ifstream input(path, std::ios::binary);
    std::string legacy((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    const auto version = legacy.find("\"version\": 3");
    if (version == std::string::npos) {
      return Fail("Current session manifest version was not serialized.");
    }
    legacy.replace(version, std::string("\"version\": 3").size(),
                   "\"version\": 2");
    input.close();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << legacy;
  }
  if (!olouie::record::ReadSessionManifest(session_dir, &recovered)
           .Succeeded() ||
      recovered.version != olouie::record::kAudioSessionManifestVersion ||
      recovered.audio_tracks.size() != 1 || !recovered.bookmarks.empty()) {
    return Fail("Version 2 H.264/AAC manifests should remain readable.");
  }

  {
    std::ofstream unsupported(olouie::record::SessionManifestPath(session_dir),
                              std::ios::binary | std::ios::trunc);
    unsupported << "{\n  \"version\": 999\n}\n";
  }
  if (olouie::record::ReadSessionManifest(session_dir, &recovered).status !=
      olouie::record::SessionManifestStatus::UnsupportedVersion) {
    return Fail("Session manifest read should reject unsupported versions.");
  }

  {
    std::ofstream malformed(olouie::record::SessionManifestPath(session_dir),
                            std::ios::binary | std::ios::trunc);
    malformed << "{\n  \"version\": 1\n}\n";
  }
  if (olouie::record::ReadSessionManifest(session_dir, &recovered).status !=
      olouie::record::SessionManifestStatus::ParseFailed) {
    return Fail("Session manifest read should reject incomplete manifests.");
  }

  if (std::wstring(olouie::record::SessionManifestStatusName(
          olouie::record::SessionManifestStatus::UnsupportedVersion)) !=
      L"unsupported version") {
    return Fail("Session manifest status names changed unexpectedly.");
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestVideoExportPlan() {
  const auto root = RecordTestRoot(L"O'LouieVideoExportPlanTests");
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
      olouie::record::TrackDefinition{2, olouie::record::CodecId::Aac},
  };

  std::wstring error;
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for video export plan failed: " << error
               << L'\n';
    return 1;
  }

  if (!Append(&store, VideoPacket(0, true), "v0") ||
      !Append(&store, AudioPacket(10 * kMs, 20 * kMs), "a10") ||
      !Append(&store, VideoPacket(33 * kMs, false), "v33") ||
      !Append(&store, AudioPacket(40 * kMs, 20 * kMs), "a40") ||
      !Append(&store, VideoPacket(66 * kMs, true), "v66") ||
      !Append(&store, AudioPacket(90 * kMs, 20 * kMs), "a90") ||
      !Append(&store, VideoPacket(99 * kMs, false), "v99")) {
    return 1;
  }

  auto manifest = MakeReadyVideoManifest(session_dir, 1);
  manifest.audio_tracks.push_back(MakeReadyAudioManifestTrack(2));
  const auto manifest_write = olouie::record::WriteSessionManifest(manifest);
  if (!manifest_write.Succeeded()) {
    std::wcerr << L"Video export test manifest write failed: "
               << manifest_write.message << L'\n';
    return 1;
  }

  store.Close();

  olouie::record::VideoExportPlanOptions options;
  options.requested_start_ns = 80 * kMs;
  options.requested_end_ns = 120 * kMs;

  olouie::record::VideoExportPlan plan;
  const auto result =
      olouie::record::BuildRecoveredVideoExportPlan(session_dir, options,
                                                    &plan);
  if (!result.Succeeded() || !plan.IsReady() || plan.video.track_id != 1 ||
      plan.video.width != 1920 || plan.video.height != 1080 ||
      plan.video.h264_avcc_extradata !=
          manifest.video.h264_avcc_extradata ||
      plan.audio_tracks.size() != 1 || plan.audio_tracks[0].track_id != 2 ||
      plan.audio_tracks[0].sample_rate != 48000 ||
      plan.audio_tracks[0].aac_audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x90}) ||
      plan.mux_plan.tracks.size() != 2 ||
      plan.mux_plan.packets.size() != 3 ||
      plan.mux_plan.source_start_ns != 66 * kMs ||
      plan.mux_plan.source_end_ns != 120 * kMs ||
      plan.mux_plan.packets[0].packet.metadata.track_id != 1 ||
      plan.mux_plan.packets[0].output_dts_ns != 0 ||
      plan.mux_plan.packets[1].packet.metadata.track_id != 2 ||
      plan.mux_plan.packets[1].output_dts_ns != 24 * kMs ||
      plan.mux_plan.packets[2].packet.metadata.track_id != 1 ||
      plan.mux_plan.packets[2].output_dts_ns != 33 * kMs) {
    std::wcerr << L"Video export plan build failed: " << result.message
               << L'\n';
    return 1;
  }

  olouie::record::Mp4MuxRequest mux_request;
  const auto mux_build = olouie::record::BuildVideoMp4MuxRequest(
      plan, root / L"exports" / L"clip.tmp",
      root / L"exports" / L"clip.mp4", false, &mux_request);
  if (!mux_build.Succeeded() ||
      mux_request.packet_file_path != session_dir / L"packets.dat" ||
      mux_request.video_track.track_id != 1 ||
      mux_request.video_track.width != 1920 ||
      mux_request.video_track.avcc_extradata !=
          manifest.video.h264_avcc_extradata ||
      mux_request.audio_tracks.size() != 1 ||
      mux_request.audio_tracks[0].track_id != 2 ||
      mux_request.audio_tracks[0].audio_specific_config !=
          std::vector<uint8_t>({0x11, 0x90}) ||
      mux_request.plan.packets.size() != plan.mux_plan.packets.size()) {
    std::wcerr << L"Video MP4 mux request build failed: "
               << mux_build.message << L'\n';
    return 1;
  }

  const olouie::record::Mp4Muxer video_muxer;
  const auto video_write = video_muxer.WriteMp4(mux_request);
  if (olouie::record::Mp4Muxer::BackendAvailability().Available()) {
    if (video_write.status != olouie::record::Mp4MuxStatus::InvalidRequest) {
      return Fail("Video MP4 writer should reject non-Annex-B test payloads.");
    }
  } else if (video_write.status !=
             olouie::record::Mp4MuxStatus::BackendUnavailable) {
    return Fail("Video MP4 mux request should stop at backend unavailable.");
  }

  olouie::record::Mp4MuxPayloadReadStats video_read_stats;
  const auto video_read =
      olouie::record::Mp4Muxer::DryRunPayloadRead(mux_request,
                                                  &video_read_stats);
  if (!video_read.Succeeded() || video_read_stats.packet_count != 3 ||
      video_read_stats.video_packet_count != 2 ||
      video_read_stats.audio_packet_count != 1 ||
      video_read_stats.payload_byte_count != 9 ||
      video_read_stats.video_payload_byte_count != 6 ||
      video_read_stats.audio_payload_byte_count != 3 ||
      video_read_stats.first_video_dts_ns != 0 ||
      video_read_stats.last_video_dts_ns != 33 * kMs) {
    std::wcerr << L"Video MP4 payload dry run failed: "
               << video_read.message << L'\n';
    return 1;
  }

  auto recovered = olouie::record::PacketStore::Recover(session_dir, &error);
  if (recovered.SnapshotIndex().empty()) {
    std::wcerr << L"PacketStore recover for video export test failed: "
               << error << L'\n';
    return 1;
  }

  auto silent_range_manifest = manifest;
  silent_range_manifest.audio_tracks.push_back(
      MakeReadyAudioManifestTrack(3, L"microphone", L"Microphone"));
  olouie::record::VideoExportPlanOptions silent_range_options;
  silent_range_options.requested_start_ns = 0;
  silent_range_options.requested_end_ns = 5 * kMs;
  olouie::record::VideoExportPlan silent_range_plan;
  const auto silent_range_result = olouie::record::BuildVideoExportPlan(
      silent_range_manifest, &recovered, silent_range_options,
      &silent_range_plan);
  olouie::record::Mp4MuxRequest silent_range_request;
  const auto silent_range_request_result =
      olouie::record::BuildVideoMp4MuxRequest(
          silent_range_plan, root / L"exports" / L"silent-range.tmp",
          root / L"exports" / L"silent-range.mp4", false,
          &silent_range_request);
  olouie::record::Mp4MuxPayloadReadStats silent_range_stats;
  const auto silent_range_read = olouie::record::Mp4Muxer::DryRunPayloadRead(
      silent_range_request, &silent_range_stats);
  if (!silent_range_result.Succeeded() || !silent_range_plan.IsReady() ||
      !silent_range_plan.audio_tracks.empty() ||
      silent_range_plan.omitted_audio_track_ids !=
          std::vector<uint32_t>({2, 3}) ||
      silent_range_plan.mux_plan.tracks.size() != 1 ||
      silent_range_plan.mux_plan.packets.size() != 1 ||
      !silent_range_request_result.Succeeded() ||
      !silent_range_request.audio_tracks.empty() ||
      !silent_range_read.Succeeded() ||
      silent_range_stats.video_packet_count != 1 ||
      silent_range_stats.audio_packet_count != 0) {
    std::wcerr << L"Silent selected-range export planning failed: "
               << silent_range_result.message << L' '
               << silent_range_request_result.message << L'\n';
    return 1;
  }

  olouie::record::VideoExportPlan partial_audio_plan;
  const auto partial_audio_result = olouie::record::BuildVideoExportPlan(
      silent_range_manifest, &recovered, options, &partial_audio_plan);
  if (!partial_audio_result.Succeeded() || !partial_audio_plan.IsReady() ||
      partial_audio_plan.audio_tracks.size() != 1 ||
      partial_audio_plan.audio_tracks[0].track_id != 2 ||
      partial_audio_plan.omitted_audio_track_ids !=
          std::vector<uint32_t>({3}) ||
      partial_audio_plan.mux_plan.tracks.size() != 2 ||
      partial_audio_plan.mux_plan.packets.size() != 3 ||
      partial_audio_plan.mux_plan.packets[1].packet.metadata.track_id != 2 ||
      partial_audio_plan.mux_plan.packets[1].output_dts_ns != 24 * kMs) {
    std::wcerr << L"Partial-audio export did not preserve packet timing: "
               << partial_audio_result.message << L'\n';
    return 1;
  }
  auto strict_audio_options = options;
  strict_audio_options.require_all_audio_tracks = true;
  olouie::record::VideoExportPlan strict_audio_plan;
  const auto strict_audio_result = olouie::record::BuildVideoExportPlan(
      silent_range_manifest, &recovered, strict_audio_options,
      &strict_audio_plan);
  if (strict_audio_result.status !=
          olouie::record::VideoExportPlanStatus::MissingAudioPackets ||
      strict_audio_result.message.find(L"Microphone") ==
          std::wstring::npos ||
      strict_audio_plan.IsReady()) {
    return Fail("Strict export should reject a missing configured audio track.");
  }
  auto invalid_omission_plan = partial_audio_plan;
  invalid_omission_plan.omitted_audio_track_ids.push_back(2);
  if (invalid_omission_plan.IsReady()) {
    return Fail("Export plan should reject included and omitted audio overlap.");
  }

  olouie::record::PacketStoreExportSnapshot wrong_audio_codec_snapshot;
  wrong_audio_codec_snapshot.session_dir = session_dir;
  wrong_audio_codec_snapshot.packet_file_path = session_dir / L"packets.dat";
  wrong_audio_codec_snapshot.index = recovered.SnapshotIndex();
  for (auto& packet : wrong_audio_codec_snapshot.index) {
    if (packet.metadata.track_id == 2) {
      packet.metadata.codec_id = olouie::record::CodecId::H264;
      break;
    }
  }
  if (olouie::record::BuildVideoExportPlan(
          manifest, wrong_audio_codec_snapshot, options, &silent_range_plan)
          .status != olouie::record::VideoExportPlanStatus::MetadataMismatch) {
    return Fail("Silent-audio handling should not hide audio codec mismatches.");
  }

  if (olouie::record::BuildVideoMp4MuxRequest(
          olouie::record::VideoExportPlan{}, root / L"exports" / L"bad.tmp",
          root / L"exports" / L"bad.mp4", false, &mux_request)
          .Succeeded()) {
    return Fail("MP4 mux request builder should reject incomplete export plans.");
  }

  if (olouie::record::Mp4Muxer::ValidateRequest(mux_request).Succeeded()) {
    return Fail("Rejected MP4 mux request should leave an empty request.");
  }

  olouie::record::VideoExportPlanOptions no_keyframe_options;
  no_keyframe_options.requested_start_ns = 100 * kMs;
  no_keyframe_options.requested_end_ns = 120 * kMs;
  no_keyframe_options.include_previous_keyframe = false;
  if (olouie::record::BuildVideoExportPlan(
          manifest, &recovered, no_keyframe_options, &plan)
          .status != olouie::record::VideoExportPlanStatus::NoKeyframe) {
    return Fail("Video export plan should reject non-keyframe starts.");
  }

  auto mismatched_manifest = manifest;
  mismatched_manifest.video.track_id = 9;
  if (olouie::record::BuildVideoExportPlan(
          mismatched_manifest, &recovered, options, &plan)
          .status != olouie::record::VideoExportPlanStatus::NoPackets) {
    return Fail("Video export plan should reject missing manifest video tracks.");
  }

  olouie::record::VideoExportPlanOptions invalid_range;
  invalid_range.requested_start_ns = 120 * kMs;
  invalid_range.requested_end_ns = 80 * kMs;
  if (olouie::record::BuildVideoExportPlan(manifest, &recovered,
                                           invalid_range, &plan)
          .status != olouie::record::VideoExportPlanStatus::InvalidRequest) {
    return Fail("Video export plan should reject invalid ranges.");
  }

  if (olouie::record::BuildRecoveredVideoExportPlan(root / L"missing", options,
                                                    &plan)
          .status !=
      olouie::record::VideoExportPlanStatus::ManifestReadFailed) {
    return Fail("Recovered video export plan should report missing manifests.");
  }

  if (std::wstring(olouie::record::VideoExportPlanStatusName(
          olouie::record::VideoExportPlanStatus::NoKeyframe)) !=
      L"no keyframe") {
    return Fail("Video export plan status names changed unexpectedly.");
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestActiveRecordingClip() {
  using namespace std::chrono_literals;
  const auto root = RecordTestRoot(L"O'LouieActiveRecordingClipTests");
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264},
      olouie::record::TrackDefinition{2, olouie::record::CodecId::Aac},
  };
  std::wstring error;
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable() ||
      !Append(&store, VideoPacket(0, true), "v0") ||
      !Append(&store, AudioPacket(0, 20 * kMs), "a0") ||
      !Append(&store, VideoPacket(33 * kMs, false), "v33") ||
      !Append(&store, AudioPacket(20 * kMs, 20 * kMs), "a20") ||
      !Append(&store, VideoPacket(66 * kMs, true), "v66") ||
      !Append(&store, AudioPacket(60 * kMs, 20 * kMs), "a60") ||
      !Append(&store, VideoPacket(99 * kMs, false), "v99")) {
    std::wcerr << L"Active clip PacketStore fixture failed: " << error
               << L'\n';
    return 1;
  }

  auto manifest = MakeReadyVideoManifest(session_dir, 1);
  manifest.audio_tracks.push_back(MakeReadyAudioManifestTrack(2));

  olouie::record::ActiveRecordingClipOptions short_options;
  short_options.duration_ns = 300 * kMs;
  short_options.temp_output_path = root / L"exports" / L"short.partial.mp4";
  short_options.final_output_path = root / L"exports" / L"short.mp4";
  olouie::record::ActiveRecordingClipPlan short_plan;
  const auto short_result = olouie::record::BuildActiveRecordingClipPlan(
      manifest, &store, short_options, &short_plan);
  if (!short_result.Succeeded() || !short_plan.IsReady() ||
      short_plan.available_end_ns != 132 * kMs ||
      short_plan.clamped_start_ns != 0 || short_plan.actual_start_ns != 0 ||
      short_plan.actual_end_ns != 132 * kMs ||
      short_plan.mux_request.audio_tracks.size() != 1) {
    std::wcerr << L"Short active clip planning failed: "
               << short_result.message << L'\n';
    return 1;
  }

  olouie::record::ActiveRecordingClipOptions aligned_options;
  aligned_options.duration_ns = 30 * kMs;
  aligned_options.temp_output_path = root / L"exports" / L"aligned.partial.mp4";
  aligned_options.final_output_path = root / L"exports" / L"aligned.mp4";
  olouie::record::ActiveRecordingClipPlan aligned_plan;
  const auto aligned_result = olouie::record::BuildActiveRecordingClipPlan(
      manifest, &store, aligned_options, &aligned_plan);
  if (!aligned_result.Succeeded() || aligned_plan.clamped_start_ns != 102 * kMs ||
      aligned_plan.actual_start_ns != 66 * kMs ||
      aligned_plan.mux_request.plan.packets.size() != 3 ||
      !aligned_plan.mux_request.plan.packets.front().packet.IsKeyframe() ||
      aligned_plan.mux_request.plan.packets[1].packet.metadata.track_id != 2 ||
      aligned_plan.mux_request.plan.packets[1].packet.metadata.pts_ns !=
          60 * kMs) {
    std::wcerr << L"Keyframe/AAC active clip planning failed: "
               << aligned_result.message << L'\n';
    return 1;
  }

  olouie::record::BookmarkCollection active_bookmarks;
  olouie::record::ActiveRecordingBookmarkOptions bookmark_options;
  bookmark_options.pre_roll_ns = 30 * kMs;
  bookmark_options.temp_output_path =
      root / L"exports" / L"bookmark.partial.mp4";
  bookmark_options.final_output_path = root / L"exports" / L"bookmark.mp4";
  olouie::record::ActiveRecordingBookmarkPlan bookmark_plan;
  const auto bookmark_result =
      olouie::record::BuildActiveRecordingBookmarkPlan(
          manifest, &store, &active_bookmarks, bookmark_options,
          &bookmark_plan);
  if (!bookmark_result.Succeeded() || !bookmark_plan.IsReady() ||
      bookmark_plan.bookmark.id != 1 ||
      bookmark_plan.bookmark.time_ns != 132 * kMs ||
      bookmark_plan.bookmark.default_pre_ns != 30 * kMs ||
      bookmark_plan.range.requested_start_ns != 102 * kMs ||
      bookmark_plan.range.actual_start_ns != 102 * kMs ||
      bookmark_plan.clip_plan.actual_start_ns != 66 * kMs ||
      bookmark_plan.clip_plan.mux_request.plan.packets.size() != 3 ||
      bookmark_plan.clip_plan.mux_request.plan.packets[1]
              .packet.metadata.codec_id != olouie::record::CodecId::Aac ||
      active_bookmarks.Count() != 1) {
    std::wcerr << L"Active bookmark planning failed: "
               << bookmark_result.message << L'\n';
    return 1;
  }

  auto post_roll_options = bookmark_options;
  post_roll_options.post_roll_ns = 10 * kMs;
  if (olouie::record::BuildActiveRecordingBookmarkPlan(
          manifest, &store, &active_bookmarks, post_roll_options,
          &bookmark_plan)
          .status != olouie::record::ActiveRecordingBookmarkStatus::
                         PostRollUnsupported ||
      active_bookmarks.Count() != 1) {
    return Fail("Unsupported active bookmark post-roll should be explicit.");
  }

  olouie::record::Mp4MuxPayloadReadStats read_stats;
  const auto read = olouie::record::Mp4Muxer::DryRunPayloadRead(
      aligned_plan.mux_request, &read_stats);
  if (!read.Succeeded() || read_stats.video_packet_count != 2 ||
      read_stats.audio_packet_count != 1) {
    return Fail("Live PacketStore payloads should be visible after snapshot flush.");
  }

  const auto silent_session_dir = root / L"silent-session";
  auto silent_store =
      olouie::record::PacketStore::Create(silent_session_dir, tracks, &error);
  if (!silent_store.IsWritable() ||
      !Append(&silent_store, VideoPacket(0, true), "sv0") ||
      !Append(&silent_store, VideoPacket(33 * kMs, false), "sv33") ||
      !Append(&silent_store, VideoPacket(66 * kMs, true), "sv66") ||
      !Append(&silent_store, VideoPacket(99 * kMs, false), "sv99")) {
    return Fail("Silent active-export PacketStore fixture failed.");
  }
  auto silent_manifest = MakeReadyVideoManifest(silent_session_dir, 1);
  silent_manifest.audio_tracks.push_back(MakeReadyAudioManifestTrack(2));
  auto silent_clip_options = short_options;
  silent_clip_options.temp_output_path =
      root / L"exports" / L"silent-clip.partial.mp4";
  silent_clip_options.final_output_path =
      root / L"exports" / L"silent-clip.mp4";
  olouie::record::ActiveRecordingClipPlan silent_clip_plan;
  const auto silent_clip_result =
      olouie::record::BuildActiveRecordingClipPlan(
          silent_manifest, &silent_store, silent_clip_options,
          &silent_clip_plan);
  olouie::record::BookmarkCollection silent_bookmarks;
  auto silent_bookmark_options = bookmark_options;
  silent_bookmark_options.temp_output_path =
      root / L"exports" / L"silent-bookmark.partial.mp4";
  silent_bookmark_options.final_output_path =
      root / L"exports" / L"silent-bookmark.mp4";
  olouie::record::ActiveRecordingBookmarkPlan silent_bookmark_plan;
  const auto silent_bookmark_result =
      olouie::record::BuildActiveRecordingBookmarkPlan(
          silent_manifest, &silent_store, &silent_bookmarks,
          silent_bookmark_options, &silent_bookmark_plan);
  if (silent_clip_result.status !=
          olouie::record::ActiveRecordingClipStatus::ExportPlanFailed ||
      silent_clip_plan.IsReady() ||
      silent_bookmark_result.status !=
          olouie::record::ActiveRecordingBookmarkStatus::ClipPlanFailed ||
      silent_bookmark_plan.IsReady() || silent_bookmarks.Count() != 0) {
    std::wcerr << L"Missing-audio active clip/bookmark rejection failed: "
               << silent_clip_result.message << L' '
               << silent_bookmark_result.message << L'\n';
    return 1;
  }
  std::mutex export_mutex;
  std::condition_variable export_ready;
  bool writer_entered = false;
  bool release_writer = false;
  std::thread::id writer_thread;
  std::vector<olouie::record::ClipExportCompletion> completions;
  olouie::record::ClipExportQueueOptions queue_options;
  queue_options.capacity = 2;
  queue_options.writer = [&](const olouie::record::Mp4MuxRequest& request) {
    {
      std::unique_lock lock(export_mutex);
      writer_thread = std::this_thread::get_id();
      writer_entered = true;
      export_ready.notify_all();
      export_ready.wait(lock, [&] { return release_writer; });
    }
    if (request.final_output_path.filename().wstring().find(L"fail") !=
        std::wstring::npos) {
      return olouie::record::Mp4MuxResult{
          olouie::record::Mp4MuxStatus::FileSystemError,
          L"injected clip export failure"};
    }
    return olouie::record::Mp4MuxResult{
        olouie::record::Mp4MuxStatus::Success, L""};
  };

  olouie::record::ClipExportQueue queue(std::move(queue_options));
  if (!queue.Start([&](const olouie::record::ClipExportCompletion& completion) {
        std::lock_guard lock(export_mutex);
        completions.push_back(completion);
      }).Succeeded()) {
    return Fail("Clip export queue did not start.");
  }

  olouie::record::ClipExportJob first_job;
  first_job.request_id = 1;
  first_job.duration = 30ms;
  first_job.output_path = aligned_options.final_output_path;
  first_job.mux_request = aligned_plan.mux_request;
  if (!queue.Enqueue(std::move(first_job)).Succeeded()) {
    return Fail("First clip export job was rejected.");
  }
  {
    std::unique_lock lock(export_mutex);
    if (!export_ready.wait_for(lock, 2s, [&] { return writer_entered; })) {
      return Fail("Clip export worker did not begin in time.");
    }
  }

  const auto index_before_append = store.SnapshotIndex().size();
  if (!Append(&store, AudioPacket(120 * kMs, 20 * kMs), "a120") ||
      !Append(&store, VideoPacket(132 * kMs, false), "v132") ||
      store.SnapshotIndex().size() != index_before_append + 2) {
    return Fail("Packet capture should continue while clip export is blocked.");
  }

  auto failed_request = aligned_plan.mux_request;
  failed_request.temp_output_path = root / L"exports" / L"fail.partial.mp4";
  failed_request.final_output_path = root / L"exports" / L"fail.mp4";
  olouie::record::ClipExportJob failed_job;
  failed_job.kind = olouie::record::VideoRecorderExportKind::Bookmark;
  failed_job.request_id = 2;
  failed_job.duration = 30ms;
  failed_job.bookmark_id = 7;
  failed_job.bookmark_time_ns = 132 * kMs;
  failed_job.output_path = failed_request.final_output_path;
  failed_job.mux_request = failed_request;
  if (!queue.Enqueue(std::move(failed_job)).Succeeded()) {
    return Fail("Second clip export job was rejected.");
  }

  auto rejected_request = aligned_plan.mux_request;
  rejected_request.temp_output_path = root / L"exports" / L"third.partial.mp4";
  rejected_request.final_output_path = root / L"exports" / L"third.mp4";
  olouie::record::ClipExportJob rejected_job;
  rejected_job.request_id = 3;
  rejected_job.duration = 30ms;
  rejected_job.output_path = rejected_request.final_output_path;
  rejected_job.mux_request = std::move(rejected_request);
  if (queue.Enqueue(std::move(rejected_job)).status !=
      olouie::record::ClipExportQueueStatus::QueueFull) {
    return Fail("Bounded clip export queue should reject excess jobs.");
  }

  {
    std::lock_guard lock(export_mutex);
    release_writer = true;
  }
  export_ready.notify_all();
  queue.Shutdown();

  const auto queue_stats = queue.Snapshot();
  if (writer_thread == std::this_thread::get_id() ||
      queue_stats.submitted_job_count != 2 ||
      queue_stats.saved_job_count != 1 ||
      queue_stats.failed_job_count != 1 ||
      queue_stats.rejected_job_count != 1 ||
      queue_stats.outstanding_job_count != 0 || queue_stats.running ||
      completions.size() != 2 || !completions[0].result.Succeeded() ||
      completions[1].result.Succeeded() ||
      completions[1].job.kind !=
          olouie::record::VideoRecorderExportKind::Bookmark ||
      completions[1].job.bookmark_id != 7 ||
      completions[1].job.bookmark_time_ns != 132 * kMs ||
      completions[1].result.message.find(L"injected") == std::wstring::npos ||
      active_bookmarks.Count() != 1) {
    return Fail("Clip export queue completion/failure/shutdown state is wrong.");
  }
  olouie::record::ClipExportJob after_shutdown;
  after_shutdown.request_id = 4;
  after_shutdown.duration = 30ms;
  after_shutdown.output_path = aligned_options.final_output_path;
  after_shutdown.mux_request = aligned_plan.mux_request;
  if (queue.Enqueue(std::move(after_shutdown)).status !=
      olouie::record::ClipExportQueueStatus::NotRunning) {
    return Fail("Clip export queue should reject valid jobs after shutdown.");
  }

  olouie::record::VideoRecorderClipCommandQueue command_queue(2);
  const auto command = command_queue.Enqueue(5min);
  const auto bookmark_command = command_queue.EnqueueBookmark(60s, 0ms);
  if (!command.Accepted() || command.request_id == 0 ||
      !bookmark_command.Accepted() ||
      bookmark_command.request_id != command.request_id + 1 ||
      command_queue.Enqueue(30s).status !=
          olouie::record::VideoRecorderClipCommandStatus::QueueFull) {
    return Fail("Recorder export command queue should bound mixed requests.");
  }
  olouie::record::VideoRecorderClipRequest popped;
  if (!command_queue.TryPop(&popped) ||
      popped.request_id != command.request_id || popped.duration != 5min ||
      popped.kind != olouie::record::VideoRecorderExportKind::Clip ||
      !command_queue.TryPop(&popped) ||
      popped.request_id != bookmark_command.request_id ||
      popped.kind != olouie::record::VideoRecorderExportKind::Bookmark ||
      popped.duration != 60s || popped.bookmark_pre_roll != 60s ||
      popped.bookmark_post_roll != 0ms) {
    return Fail("Recorder export queue did not preserve typed FIFO data.");
  }
  if (command_queue.EnqueueBookmark(0ms, 0ms).status !=
      olouie::record::VideoRecorderClipCommandStatus::InvalidDuration) {
    return Fail("Bookmark command queue should reject an empty window.");
  }
  command_queue.Shutdown();
  if (command_queue.EnqueueBookmark(60s, 0ms).status !=
      olouie::record::VideoRecorderClipCommandStatus::ShuttingDown) {
    return Fail("Export command queue shutdown should reject bookmark work.");
  }

  store.Close();
  silent_store.Close();
  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

class FakeVideoRecorderPipelineBackend final
    : public olouie::record::IVideoRecorderPipelineBackend {
 public:
  olouie::record::VideoRecorderPipelineStage fail_stage =
      olouie::record::VideoRecorderPipelineStage::Complete;
  olouie::record::VideoRecorderPipelineFailureDisposition
      failure_disposition =
          olouie::record::VideoRecorderPipelineFailureDisposition::Abort;
  std::wstring failure_message = L"injected pipeline failure";
  olouie::record::DiskWriteFault failure_write_fault;
  olouie::record::VideoRecorderPipelineStage secondary_fail_stage =
      olouie::record::VideoRecorderPipelineStage::Complete;
  olouie::record::VideoRecorderPipelineFailureDisposition
      secondary_failure_disposition =
          olouie::record::VideoRecorderPipelineFailureDisposition::Abort;
  std::wstring secondary_failure_message = L"secondary injected failure";
  olouie::record::DiskWriteFault secondary_failure_write_fault;
  std::atomic_bool* stop_on_capture_start = nullptr;
  size_t recording_call_count = 0;
  std::vector<olouie::record::VideoRecorderPipelineStage> calls;

  olouie::record::VideoRecorderPipelineStepResult CheckBackend() override {
    return Step(olouie::record::VideoRecorderPipelineStage::BackendCheck);
  }
  olouie::record::VideoRecorderPipelineStepResult Prepare() override {
    return Step(olouie::record::VideoRecorderPipelineStage::Prepare);
  }
  olouie::record::VideoRecorderPipelineStepResult StartCapture() override {
    auto result =
        Step(olouie::record::VideoRecorderPipelineStage::CaptureStart);
    if (result.succeeded && stop_on_capture_start != nullptr) {
      stop_on_capture_start->store(true);
    }
    return result;
  }
  olouie::record::VideoRecorderPipelineStepResult DrainCaptureTick() override {
    ++recording_call_count;
    return Step(olouie::record::VideoRecorderPipelineStage::Recording);
  }
  olouie::record::VideoRecorderPipelineStepResult ProcessClipRequests() override {
    return olouie::record::VideoRecorderPipelineStepResult::Success();
  }
  olouie::record::VideoRecorderPipelineStepResult StopCapture() override {
    return Step(olouie::record::VideoRecorderPipelineStage::CaptureStop);
  }
  olouie::record::VideoRecorderPipelineStepResult DrainQueuedFrames() override {
    return Step(olouie::record::VideoRecorderPipelineStage::QueueDrain);
  }
  olouie::record::VideoRecorderPipelineStepResult DrainEncoder() override {
    return Step(olouie::record::VideoRecorderPipelineStage::EncoderDrain);
  }
  olouie::record::VideoRecorderPipelineStepResult DrainClipExports() override {
    return Step(olouie::record::VideoRecorderPipelineStage::ClipExportDrain);
  }
  olouie::record::VideoRecorderPipelineStepResult WriteManifest() override {
    return Step(olouie::record::VideoRecorderPipelineStage::ManifestWrite);
  }
  olouie::record::VideoRecorderPipelineStepResult ClosePacketStore() override {
    return Step(olouie::record::VideoRecorderPipelineStage::PacketStoreClose);
  }
  olouie::record::VideoRecorderPipelineStepResult RecoverPacketStore() override {
    return Step(olouie::record::VideoRecorderPipelineStage::PacketStoreRecover);
  }
  olouie::record::VideoRecorderPipelineStepResult BuildExportPlan() override {
    return Step(olouie::record::VideoRecorderPipelineStage::ExportPlan);
  }
  olouie::record::VideoRecorderPipelineStepResult WriteMp4() override {
    return Step(olouie::record::VideoRecorderPipelineStage::Mp4Write);
  }
  std::filesystem::path session_directory() const override {
    return L"fake-session";
  }
  std::filesystem::path output_path() const override {
    return L"fake-output.mp4";
  }
  olouie::record::VideoRecorderPipelineStats stats() const override {
    olouie::record::VideoRecorderPipelineStats stats;
    stats.captured_frame_count = 10;
    stats.accepted_frame_count = 9;
    stats.dropped_frame_count = 1;
    stats.encoded_frame_count = 9;
    stats.encoded_packet_count = 9;
    return stats;
  }

 private:
  olouie::record::VideoRecorderPipelineStepResult Step(
      olouie::record::VideoRecorderPipelineStage stage) {
    calls.push_back(stage);
    if (stage == fail_stage) {
      return olouie::record::VideoRecorderPipelineStepResult::Failure(
          failure_message, failure_disposition, failure_write_fault);
    }
    if (stage == secondary_fail_stage) {
      return olouie::record::VideoRecorderPipelineStepResult::Failure(
          secondary_failure_message, secondary_failure_disposition,
          secondary_failure_write_fault);
    }
    return olouie::record::VideoRecorderPipelineStepResult::Success();
  }
};

bool WaitForRecorderState(olouie::record::VideoRecorderSession* session,
                          olouie::record::VideoRecorderState state) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (session->Snapshot().state == state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool CreateRecoveryFixture(const std::filesystem::path& session_dir,
                           bool write_manifest = true) {
  const std::array tracks{
      olouie::record::TrackDefinition{1, olouie::record::CodecId::H264}};
  std::wstring error;
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable() ||
      !Append(&store, VideoPacket(0, true), "keyframe") ||
      !Append(&store, VideoPacket(33 * kMs, false), "delta") ||
      !store.Close(&error)) {
    std::wcerr << L"Recovery fixture PacketStore failed: " << error << L'\n';
    return false;
  }
  if (!write_manifest) {
    return true;
  }
  const auto written = olouie::record::WriteSessionManifest(
      MakeReadyVideoManifest(session_dir, 1));
  if (!written.Succeeded()) {
    std::wcerr << L"Recovery fixture manifest failed: " << written.message
               << L'\n';
    return false;
  }
  return true;
}

bool RewriteManifestVersion(const std::filesystem::path& manifest_path,
                            uint32_t version) {
  std::ifstream input(manifest_path, std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  const std::string current = "\"version\": 3";
  const auto position = json.find(current);
  if (position == std::string::npos || version > 9) {
    return false;
  }
  json.replace(position, current.size(),
               "\"version\": " + std::to_string(version));
  std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
  output.write(json.data(), static_cast<std::streamsize>(json.size()));
  return static_cast<bool>(output);
}

bool WriteCompleteMp4Fixture(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const auto write_box = [&output](const char type[4], uint32_t payload_size) {
    const uint32_t box_size = 8 + payload_size;
    const std::array<unsigned char, 8> box{
        static_cast<unsigned char>((box_size >> 24) & 0xff),
        static_cast<unsigned char>((box_size >> 16) & 0xff),
        static_cast<unsigned char>((box_size >> 8) & 0xff),
        static_cast<unsigned char>(box_size & 0xff),
        static_cast<unsigned char>(type[0]),
        static_cast<unsigned char>(type[1]),
        static_cast<unsigned char>(type[2]),
        static_cast<unsigned char>(type[3])};
    output.write(reinterpret_cast<const char*>(box.data()), box.size());
    const std::array<char, 8> payload{};
    output.write(payload.data(), payload_size);
  };
  write_box("ftyp", 8);
  write_box("mdat", 4);
  write_box("moov", 4);
  return static_cast<bool>(output);
}

bool WaitForRecoveryState(
    olouie::record::RecordingRecoverySession* session,
    olouie::record::RecordingRecoveryState state,
    uint64_t minimum_action_generation = 0) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = session->Snapshot();
    if (snapshot.state == state &&
        snapshot.action_generation >= minimum_action_generation) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

int TestRecordingRecovery() {
  using olouie::record::RecordingRecoveryActionKind;
  using olouie::record::RecordingRecoveryActionStatus;
  using olouie::record::RecordingRecoveryCandidate;
  using olouie::record::RecordingRecoveryKind;
  using olouie::record::RecordingRecoveryScanOptions;
  using olouie::record::RecordingRecoveryScanResult;
  using olouie::record::RecordingRecoverySession;
  using olouie::record::RecordingRecoveryState;

  const auto root = RecordTestRoot(L"O'LouieRecordingRecoveryTests");
  const auto sessions = root / L"sessions";
  const auto exports = root / L"exports";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(exports);

  const auto complete = sessions / L"recording-001-complete";
  const auto prefix = sessions / L"recording-002-prefix";
  const auto legacy = sessions / L"recording-003-legacy";
  const auto manifest_temp = sessions / L"recording-004-manifest-temp";
  const auto missing = sessions / L"recording-005-missing";
  const auto corrupt = sessions / L"recording-006-corrupt";
  const auto mp4_temp = sessions / L"recording-007-temp-mp4";
  const auto exported = sessions / L"recording-008-exported";
  if (!CreateRecoveryFixture(complete) || !CreateRecoveryFixture(prefix) ||
      !CreateRecoveryFixture(legacy) ||
      !CreateRecoveryFixture(manifest_temp) ||
      !CreateRecoveryFixture(missing, false) ||
      !CreateRecoveryFixture(corrupt) ||
      !CreateRecoveryFixture(mp4_temp, false) ||
      !CreateRecoveryFixture(exported)) {
    return 1;
  }

  {
    std::ofstream tail(prefix / L"packets.dat",
                       std::ios::binary | std::ios::app);
    tail << "truncated";
  }
  if (!RewriteManifestVersion(
          olouie::record::SessionManifestPath(legacy),
          olouie::record::kLegacyVideoOnlySessionManifestVersion)) {
    return Fail("Could not create a legacy recovery manifest fixture.");
  }
  auto final_manifest = olouie::record::SessionManifestPath(manifest_temp);
  auto temporary_manifest = final_manifest;
  temporary_manifest += L".tmp";
  std::filesystem::rename(final_manifest, temporary_manifest);
  {
    std::fstream packet(corrupt / L"packets.dat",
                        std::ios::binary | std::ios::in | std::ios::out);
    packet.put('X');
  }
  const auto complete_temp_mp4 =
      exports / L"O'Louie-007-temp-mp4.partial.mp4";
  if (!WriteCompleteMp4Fixture(complete_temp_mp4)) {
    return Fail("Could not create the unpublished MP4 recovery fixture.");
  }
  {
    std::ofstream incomplete_mp4(
        exports / L"O'Louie-005-missing.partial.mp4", std::ios::binary);
    incomplete_mp4 << "incomplete MP4";
  }
  {
    std::ofstream prior_output(exports / L"O'Louie-008-exported.mp4",
                               std::ios::binary);
    prior_output << "existing";
  }

  RecordingRecoveryScanOptions options;
  options.session_root_directory = sessions;
  options.output_directory = exports;
  options.maximum_session_directories = 16;
  const auto scan = olouie::record::ScanRecordingSessions(options);
  const auto repeated = olouie::record::ScanRecordingSessions(options);
  if (!scan.succeeded || scan.truncated || scan.discovered_session_count != 8 ||
      scan.scanned_session_count != 8 || scan.candidates.size() != 8 ||
      repeated.candidates.size() != scan.candidates.size() ||
      scan.ExportableCount() != 5 || scan.DiscardableCount() != 7) {
    return Fail("Recording recovery scan summary is incorrect.");
  }

  const std::array expected{
      RecordingRecoveryKind::Complete,
      RecordingRecoveryKind::RecoverablePrefix,
      RecordingRecoveryKind::Complete,
      RecordingRecoveryKind::IncompleteMetadata,
      RecordingRecoveryKind::IncompleteMetadata,
      RecordingRecoveryKind::Corrupt,
      RecordingRecoveryKind::IncompleteMetadata,
      RecordingRecoveryKind::AlreadyExported};
  for (size_t index = 0; index < expected.size(); ++index) {
    if (scan.candidates[index].kind != expected[index] ||
        repeated.candidates[index].kind != expected[index] ||
        repeated.candidates[index].session_directory !=
            scan.candidates[index].session_directory) {
      return Fail("Recording recovery classification is not deterministic.");
    }
  }
  if (scan.candidates[1].trailing_packet_bytes != 9 ||
      !scan.candidates[3].uses_temporary_manifest ||
      scan.candidates[4].can_export ||
      scan.candidates[5].can_export ||
      !scan.candidates[6].has_complete_temporary_mp4 ||
      !scan.candidates[6].can_export ||
      scan.first_error.find(L"recording-005-missing") ==
          std::wstring::npos) {
    return Fail("Recording recovery facts or first-error retention are incorrect.");
  }

  auto bounded_options = options;
  bounded_options.maximum_session_directories = 3;
  const auto bounded =
      olouie::record::ScanRecordingSessions(bounded_options);
  if (!bounded.succeeded || !bounded.truncated ||
      bounded.discovered_session_count != 8 ||
      bounded.scanned_session_count != 3) {
    return Fail("Recording recovery scan bound is not enforced.");
  }

  const auto published =
      olouie::record::ExportRecoveredRecording(scan.candidates[6]);
  if (!published.Succeeded() ||
      published.output_path != exports / L"O'Louie-007-temp-mp4.mp4" ||
      !std::filesystem::exists(published.output_path) ||
      std::filesystem::exists(complete_temp_mp4) ||
      !std::filesystem::exists(mp4_temp)) {
    return Fail("Complete unpublished MP4 recovery failed.");
  }

  {
    std::ofstream duplicate(scan.candidates[0].final_output_path,
                            std::ios::binary);
    duplicate << "do not replace";
  }
  const auto duplicate =
      olouie::record::ExportRecoveredRecording(scan.candidates[0]);
  if (duplicate.status != RecordingRecoveryActionStatus::DestinationExists) {
    return Fail("Recovery export should protect an existing destination.");
  }

  const auto manifest_retry =
      olouie::record::ExportRecoveredRecording(scan.candidates[3]);
  const auto after_manifest_publish =
      olouie::record::ScanRecordingSessions(options);
  const auto retry_candidate = std::find_if(
      after_manifest_publish.candidates.begin(),
      after_manifest_publish.candidates.end(),
      [&manifest_temp](const auto& candidate) {
        return candidate.session_directory == manifest_temp;
      });
  if (manifest_retry.Succeeded() ||
      !std::filesystem::exists(final_manifest) ||
      std::filesystem::exists(temporary_manifest) ||
      retry_candidate == after_manifest_publish.candidates.end() ||
      retry_candidate->kind != RecordingRecoveryKind::Complete ||
      !retry_candidate->can_export) {
    return Fail("Published manifest recovery should remain retryable after mux failure.");
  }

  const auto discarded =
      olouie::record::DiscardRecoveredRecording(scan.candidates[5]);
  if (!discarded.Succeeded() || std::filesystem::exists(corrupt) ||
      !std::filesystem::exists(discarded.retained_session_path) ||
      discarded.retained_session_path.parent_path().filename() !=
          L"discarded") {
    return Fail("Recovery discard should retain the session reversibly.");
  }

  RecordingRecoveryCandidate managed_candidate;
  managed_candidate.session_directory = L"managed-session";
  managed_candidate.can_export = true;
  managed_candidate.can_discard = true;
  auto scan_calls = std::make_shared<std::atomic_uint32_t>(0);
  auto scan_runner = [scan_calls, managed_candidate](
                         const RecordingRecoveryScanOptions&) {
    RecordingRecoveryScanResult result;
    result.succeeded = true;
    if (scan_calls->fetch_add(1) == 0) {
      result.candidates.push_back(managed_candidate);
    }
    result.discovered_session_count = result.candidates.size();
    result.scanned_session_count = result.candidates.size();
    return result;
  };
  auto export_runner = [](const RecordingRecoveryCandidate&) {
    olouie::record::RecordingRecoveryActionResult result;
    result.status = RecordingRecoveryActionStatus::Success;
    result.output_path = L"managed.mp4";
    result.message = L"managed export succeeded";
    return result;
  };
  RecordingRecoverySession managed(options, scan_runner, export_runner);
  if (!managed.StartScan().Accepted() ||
      !WaitForRecoveryState(&managed, RecordingRecoveryState::Ready) ||
      managed.Snapshot().scan.ExportableCount() != 1 ||
      !managed.ExportFirst().Accepted() ||
      !WaitForRecoveryState(&managed, RecordingRecoveryState::Ready, 1)) {
    return Fail("Recording recovery worker state transition failed.");
  }
  const auto managed_done = managed.Snapshot();
  if (managed_done.action_kind != RecordingRecoveryActionKind::Export ||
      !managed_done.action.Succeeded() ||
      managed_done.action.output_path != L"managed.mp4" ||
      managed_done.scan.ExportableCount() != 0 ||
      managed.DiscardFirst().Accepted()) {
    return Fail("Recording recovery worker result or rescan is incorrect.");
  }
  managed.Shutdown();

  auto failing_export = [](const RecordingRecoveryCandidate&) {
    olouie::record::RecordingRecoveryActionResult result;
    result.status = RecordingRecoveryActionStatus::MuxFailed;
    result.message = L"injected recovery mux failure";
    return result;
  };
  auto stable_scan = [managed_candidate](const RecordingRecoveryScanOptions&) {
    RecordingRecoveryScanResult result;
    result.succeeded = true;
    result.discovered_session_count = 1;
    result.scanned_session_count = 1;
    result.candidates.push_back(managed_candidate);
    return result;
  };
  RecordingRecoverySession retryable(options, stable_scan, failing_export);
  if (!retryable.StartScan().Accepted() ||
      !WaitForRecoveryState(&retryable, RecordingRecoveryState::Ready) ||
      !retryable.ExportFirst().Accepted() ||
      !WaitForRecoveryState(&retryable, RecordingRecoveryState::Ready, 1)) {
    return Fail("Retryable recovery failure state transition failed.");
  }
  const auto failed = retryable.Snapshot();
  if (failed.action.Succeeded() ||
      failed.action.message.find(L"injected") == std::wstring::npos ||
      failed.scan.ExportableCount() != 1 ||
      !retryable.ExportFirst().Accepted()) {
    return Fail("Failed recovery should retain the candidate for retry.");
  }
  retryable.Shutdown();

  if (olouie::record::Mp4Muxer::BackendAvailability().Available()) {
    const auto real_session = sessions / L"recording-009-real-prefix";
    const std::array tracks{
        olouie::record::TrackDefinition{1, olouie::record::CodecId::H264}};
    std::wstring error;
    auto store =
        olouie::record::PacketStore::Create(real_session, tracks, &error);
    if (!store.IsWritable() ||
        !Append(&store, VideoPacket(0, true),
                {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
                 0x00, 0x00, 0x01,       0x68, 0xce, 0x06,
                 0x00, 0x00, 0x01,       0x65, 0x88, 0x84}) ||
        !Append(&store, VideoPacket(33 * kMs, false),
                {0x00, 0x00, 0x01, 0x41, 0x9a, 0x22}) ||
        !store.Close(&error) ||
        !olouie::record::WriteSessionManifest(
             MakeReadyVideoManifest(real_session, 1))
             .Succeeded()) {
      return Fail("Real recovery PacketStore fixture failed.");
    }
    {
      std::ofstream interrupted(real_session / L"packets.dat",
                                std::ios::binary | std::ios::app);
      interrupted << "interrupted-tail";
    }

    const auto real_scan = olouie::record::ScanRecordingSessions(options);
    const auto found = std::find_if(
        real_scan.candidates.begin(), real_scan.candidates.end(),
        [&real_session](const auto& candidate) {
          return candidate.session_directory == real_session;
        });
    if (found == real_scan.candidates.end() ||
        found->kind != RecordingRecoveryKind::RecoverablePrefix ||
        found->trailing_packet_bytes != 16 || !found->can_export) {
      return Fail("Real interrupted session was not classified as a recoverable prefix.");
    }
    const auto recovered =
        olouie::record::ExportRecoveredRecording(*found);
    std::error_code size_error;
    const auto output_size =
        std::filesystem::file_size(recovered.output_path, size_error);
    if (!recovered.Succeeded() || size_error || output_size == 0 ||
        !std::filesystem::exists(real_session)) {
      std::wcerr << L"Real interrupted session recovery failed: "
                 << recovered.message << L'\n';
      return 1;
    }
  }

  RemoveRecordTestRootIfTemporary(root);
  return 0;
}

int TestVideoRecorderPipelineOrdering() {
  using olouie::record::VideoRecorderPipelineStage;
  using olouie::record::VideoRecorderPipelineStatus;

  olouie::record::VideoRecorderPipelineOptions options;
  options.drain_interval = std::chrono::milliseconds(1);
  std::atomic_bool stop_requested{false};
  FakeVideoRecorderPipelineBackend backend;
  backend.stop_on_capture_start = &stop_requested;
  std::vector<VideoRecorderPipelineStage> stages;
  std::vector<olouie::record::VideoRecorderPipelineProgress> progress;
  options.progress_sink =
      [&progress](const auto& value) { progress.push_back(value); };

  const auto result = olouie::record::RunVideoRecorderPipelineWithBackend(
      options, &stop_requested, &backend,
      [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  const std::vector expected_stages{
      VideoRecorderPipelineStage::BackendCheck,
      VideoRecorderPipelineStage::Prepare,
      VideoRecorderPipelineStage::CaptureStart,
      VideoRecorderPipelineStage::Recording,
      VideoRecorderPipelineStage::CaptureStop,
      VideoRecorderPipelineStage::QueueDrain,
      VideoRecorderPipelineStage::EncoderDrain,
      VideoRecorderPipelineStage::ClipExportDrain,
      VideoRecorderPipelineStage::ManifestWrite,
      VideoRecorderPipelineStage::PacketStoreClose,
      VideoRecorderPipelineStage::PacketStoreRecover,
      VideoRecorderPipelineStage::ExportPlan,
      VideoRecorderPipelineStage::Mp4Write,
      VideoRecorderPipelineStage::Complete,
  };
  if (!result.Succeeded() || result.status != VideoRecorderPipelineStatus::Success ||
      stages != expected_stages || result.output_path != L"fake-output.mp4" ||
      result.stats.encoded_packet_count != 9 || progress.size() != 1 ||
      progress.front().session_directory != L"fake-session" ||
      progress.front().stats.captured_frame_count != 10 ||
      !result.stats.runtime.recorder_scheduling.attempted ||
      result.stats.runtime.recorder_scheduling.plan.workload !=
          olouie::performance::MultimediaThreadWorkload::Recorder ||
      result.stats.runtime.performance_mode !=
          olouie::performance::CapturePerformanceMode::Balanced ||
      result.stats.runtime.realtime_process_priority ||
      progress.front().stats.runtime.realtime_process_priority) {
    return Fail("Video recorder pipeline finalization order is incorrect.");
  }

  auto capture_first_options = options;
  capture_first_options.progress_sink = {};
  capture_first_options.performance_mode =
      olouie::performance::CapturePerformanceMode::CaptureFirst;
  capture_first_options.preflight.encoder_config.performance_mode =
      olouie::performance::CapturePerformanceMode::CaptureFirst;
  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend capture_first_backend;
  capture_first_backend.stop_on_capture_start = &stop_requested;
  const auto capture_first =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          capture_first_options, &stop_requested, &capture_first_backend);
  if (!capture_first.Succeeded() ||
      capture_first.stats.runtime.performance_mode !=
          olouie::performance::CapturePerformanceMode::CaptureFirst ||
      capture_first.stats.runtime.recorder_scheduling.plan.relative_priority !=
          olouie::performance::MultimediaThreadRelativePriority::High ||
      capture_first.stats.runtime.realtime_process_priority) {
    return Fail("Capture First pipeline scheduling is incorrect.");
  }

  auto mismatched_options = capture_first_options;
  mismatched_options.preflight.encoder_config.performance_mode =
      olouie::performance::CapturePerformanceMode::Balanced;
  FakeVideoRecorderPipelineBackend mismatched_backend;
  stop_requested.store(false);
  const auto mismatched =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          mismatched_options, &stop_requested, &mismatched_backend);
  if (mismatched.status != VideoRecorderPipelineStatus::InvalidConfig ||
      !mismatched_backend.calls.empty()) {
    return Fail("Recorder should reject mismatched performance policies.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend start_failure;
  start_failure.fail_stage = VideoRecorderPipelineStage::CaptureStart;
  stages.clear();
  const auto failed = olouie::record::RunVideoRecorderPipelineWithBackend(
      options, &stop_requested, &start_failure,
      [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  const std::vector expected_failure_stages{
      VideoRecorderPipelineStage::BackendCheck,
      VideoRecorderPipelineStage::Prepare,
      VideoRecorderPipelineStage::CaptureStart,
      VideoRecorderPipelineStage::CaptureStop,
      VideoRecorderPipelineStage::ClipExportDrain,
      VideoRecorderPipelineStage::PacketStoreClose,
  };
  if (failed.status != VideoRecorderPipelineStatus::CaptureStartFailed ||
      stages != expected_failure_stages ||
      start_failure.calls.back() !=
          VideoRecorderPipelineStage::PacketStoreClose) {
    return Fail("Video recorder pipeline partial-start cleanup is incorrect.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend runtime_failure;
  runtime_failure.fail_stage = VideoRecorderPipelineStage::Recording;
  stages.clear();
  const auto runtime_failed =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          options, &stop_requested, &runtime_failure,
          [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  const std::vector expected_runtime_failure_stages{
      VideoRecorderPipelineStage::BackendCheck,
      VideoRecorderPipelineStage::Prepare,
      VideoRecorderPipelineStage::CaptureStart,
      VideoRecorderPipelineStage::Recording,
      VideoRecorderPipelineStage::CaptureStop,
      VideoRecorderPipelineStage::QueueDrain,
      VideoRecorderPipelineStage::EncoderDrain,
      VideoRecorderPipelineStage::ClipExportDrain,
      VideoRecorderPipelineStage::PacketStoreClose,
  };
  if (runtime_failed.status != VideoRecorderPipelineStatus::CaptureFailed ||
      stages != expected_runtime_failure_stages ||
      runtime_failed.recording_saved_after_failure) {
    return Fail("Recorder runtime failures should stop, drain, flush, and close.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend topology_failure;
  topology_failure.fail_stage = VideoRecorderPipelineStage::Recording;
  topology_failure.failure_disposition =
      olouie::record::VideoRecorderPipelineFailureDisposition::
          FinalizeRecording;
  topology_failure.failure_message =
      L"The selected monitor resized from 1920x1080 to 1280x720.";
  stages.clear();
  const auto topology_failed =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          options, &stop_requested, &topology_failure,
          [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  const std::vector expected_topology_failure_stages{
      VideoRecorderPipelineStage::BackendCheck,
      VideoRecorderPipelineStage::Prepare,
      VideoRecorderPipelineStage::CaptureStart,
      VideoRecorderPipelineStage::Recording,
      VideoRecorderPipelineStage::CaptureStop,
      VideoRecorderPipelineStage::QueueDrain,
      VideoRecorderPipelineStage::EncoderDrain,
      VideoRecorderPipelineStage::ClipExportDrain,
      VideoRecorderPipelineStage::ManifestWrite,
      VideoRecorderPipelineStage::PacketStoreClose,
      VideoRecorderPipelineStage::PacketStoreRecover,
      VideoRecorderPipelineStage::ExportPlan,
      VideoRecorderPipelineStage::Mp4Write,
      VideoRecorderPipelineStage::Complete,
  };
  if (topology_failed.status != VideoRecorderPipelineStatus::CaptureFailed ||
      topology_failed.failed_stage != VideoRecorderPipelineStage::Recording ||
      !topology_failed.recording_saved_after_failure ||
      topology_failed.output_path != L"fake-output.mp4" ||
      topology_failed.message.find(L"selected monitor resized") ==
          std::wstring::npos ||
      topology_failed.message.find(L"fake-output.mp4") == std::wstring::npos ||
      topology_failure.recording_call_count != 1 ||
      stages != expected_topology_failure_stages) {
    return Fail("Monitor topology failure should finalize and preserve recording data once.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend stop_failure;
  stop_failure.stop_on_capture_start = &stop_requested;
  stop_failure.fail_stage = VideoRecorderPipelineStage::CaptureStop;
  stop_failure.failure_disposition =
      olouie::record::VideoRecorderPipelineFailureDisposition::
          FinalizeRecording;
  stop_failure.failure_message =
      L"System loopback source failed while stopping.";
  stages.clear();
  const auto stop_failed =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          options, &stop_requested, &stop_failure,
          [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  if (stop_failed.status != VideoRecorderPipelineStatus::CaptureStopFailed ||
      stop_failed.failed_stage != VideoRecorderPipelineStage::CaptureStop ||
      !stop_failed.recording_saved_after_failure ||
      stop_failed.output_path != L"fake-output.mp4" ||
      stop_failed.message.find(L"System loopback source failed") ==
          std::wstring::npos ||
      stop_failed.message.find(L"fake-output.mp4") == std::wstring::npos ||
      stages != expected_topology_failure_stages) {
    return Fail("Capture-stop failures should preserve the recording and first diagnosis.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend device_failure;
  device_failure.stop_on_capture_start = &stop_requested;
  device_failure.fail_stage = VideoRecorderPipelineStage::QueueDrain;
  device_failure.failure_disposition =
      olouie::record::VideoRecorderPipelineFailureDisposition::
          FinalizeRecording;
  device_failure.failure_message =
      L"The D3D11 device was reset during video processor blit.";
  device_failure.secondary_fail_stage =
      VideoRecorderPipelineStage::EncoderDrain;
  device_failure.secondary_failure_disposition =
      olouie::record::VideoRecorderPipelineFailureDisposition::
          FinalizeRecording;
  device_failure.secondary_failure_message =
      L"Hardware H.264 encoder drain also failed.";
  stages.clear();
  const auto device_failed =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          options, &stop_requested, &device_failure,
          [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  if (device_failed.status != VideoRecorderPipelineStatus::QueueDrainFailed ||
      device_failed.failed_stage != VideoRecorderPipelineStage::QueueDrain ||
      !device_failed.recording_saved_after_failure ||
      device_failed.message.find(L"D3D11 device was reset") ==
          std::wstring::npos ||
      device_failed.message.find(L"encoder drain also failed") !=
          std::wstring::npos ||
      device_failed.message.find(L"fake-output.mp4") == std::wstring::npos ||
      stages != expected_topology_failure_stages) {
    return Fail("Device loss during final drain should preserve packets and the first diagnosis.");
  }

  stop_requested.store(false);
  FakeVideoRecorderPipelineBackend disk_failure;
  disk_failure.fail_stage = VideoRecorderPipelineStage::Recording;
  disk_failure.failure_disposition =
      olouie::record::VideoRecorderPipelineFailureDisposition::
          FinalizeRecording;
  disk_failure.failure_write_fault = olouie::record::MakeDiskWriteFault(
      olouie::record::DiskWriteSubsystem::PacketStore,
      olouie::record::DiskWriteOperation::Append,
      L"fake-session\\packets.dat",
      std::make_error_code(std::errc::no_space_on_device));
  disk_failure.failure_message = olouie::record::DescribeDiskWriteFault(
      disk_failure.failure_write_fault);
  disk_failure.secondary_fail_stage = VideoRecorderPipelineStage::Mp4Write;
  disk_failure.secondary_failure_write_fault =
      olouie::record::MakeDiskWriteFault(
          olouie::record::DiskWriteSubsystem::Mp4Mux,
          olouie::record::DiskWriteOperation::AtomicPublish,
          L"fake-output.mp4",
          std::make_error_code(std::errc::permission_denied));
  disk_failure.secondary_failure_message =
      olouie::record::DescribeDiskWriteFault(
          disk_failure.secondary_failure_write_fault);
  stages.clear();
  const auto disk_failed =
      olouie::record::RunVideoRecorderPipelineWithBackend(
          options, &stop_requested, &disk_failure,
          [&](VideoRecorderPipelineStage stage) { stages.push_back(stage); });
  const std::vector expected_disk_failure_stages{
      VideoRecorderPipelineStage::BackendCheck,
      VideoRecorderPipelineStage::Prepare,
      VideoRecorderPipelineStage::CaptureStart,
      VideoRecorderPipelineStage::Recording,
      VideoRecorderPipelineStage::CaptureStop,
      VideoRecorderPipelineStage::QueueDrain,
      VideoRecorderPipelineStage::EncoderDrain,
      VideoRecorderPipelineStage::ClipExportDrain,
      VideoRecorderPipelineStage::ManifestWrite,
      VideoRecorderPipelineStage::PacketStoreClose,
      VideoRecorderPipelineStage::PacketStoreRecover,
      VideoRecorderPipelineStage::ExportPlan,
      VideoRecorderPipelineStage::Mp4Write,
  };
  if (disk_failed.status != VideoRecorderPipelineStatus::Mp4WriteFailed ||
      disk_failed.failed_stage != VideoRecorderPipelineStage::Mp4Write ||
      disk_failed.recording_saved_after_failure ||
      disk_failed.write_fault.kind !=
          olouie::record::DiskWriteFaultKind::NoSpace ||
      disk_failed.write_fault.subsystem !=
          olouie::record::DiskWriteSubsystem::PacketStore ||
      disk_failed.write_fault.path != L"fake-session\\packets.dat" ||
      disk_failed.message.find(L"out of disk space") ==
          std::wstring::npos ||
      disk_failed.message.find(L"atomic publication") ==
          std::wstring::npos ||
      disk_failure.recording_call_count != 1 ||
      stages != expected_disk_failure_stages) {
    return Fail("Disk failure finalization should retain the first typed fault and complete cleanup in order.");
  }

  if (std::wstring(olouie::record::VideoRecorderPipelineStageName(
          VideoRecorderPipelineStage::EncoderDrain)) != L"encoder drain" ||
      std::wstring(olouie::record::VideoRecorderPipelineStatusName(
          VideoRecorderPipelineStatus::Mp4WriteFailed)) != L"MP4 write failed") {
    return Fail("Video recorder pipeline status names changed unexpectedly.");
  }
  return 0;
}

int TestVideoRecorderSessionState() {
  using olouie::record::VideoRecorderPipelineResult;
  using olouie::record::VideoRecorderPipelineStage;
  using olouie::record::VideoRecorderPipelineStatus;
  using olouie::record::VideoRecorderState;

  olouie::record::VideoRecorderPipelineOptions options;
  options.drain_interval = std::chrono::milliseconds(1);
  auto runner = [](const olouie::record::VideoRecorderPipelineOptions& options,
                   std::atomic_bool* stop_requested,
                   olouie::record::VideoRecorderPipelineStageSink sink) {
    sink(VideoRecorderPipelineStage::Recording);
    if (options.progress_sink) {
      olouie::record::VideoRecorderPipelineProgress progress;
      progress.session_directory = L"active-session";
      progress.output_path = L"active-output.mp4";
      progress.stats.captured_frame_count = 60;
      progress.stats.runtime.recording_elapsed_ns = 1000000000ULL;
      options.progress_sink(progress);
    }
    while (!stop_requested->load()) {
      olouie::record::VideoRecorderClipRequest request;
      if (options.clip_commands != nullptr &&
          options.clip_commands->TryPop(&request) &&
          options.clip_event_sink) {
        olouie::record::VideoRecorderClipEvent event;
        const bool injected_failure =
            (request.kind == olouie::record::VideoRecorderExportKind::Clip &&
             request.duration == std::chrono::seconds(31)) ||
            (request.kind ==
                 olouie::record::VideoRecorderExportKind::Bookmark &&
             request.bookmark_pre_roll == std::chrono::seconds(61));
        event.kind = request.kind;
        event.state = injected_failure
                           ? olouie::record::VideoRecorderClipState::Failed
                           : olouie::record::VideoRecorderClipState::Saved;
        event.request_id = request.request_id;
        event.duration = request.duration;
        event.bookmark_id =
            request.kind == olouie::record::VideoRecorderExportKind::Bookmark
                ? 7
                : 0;
        event.bookmark_time_ns = event.bookmark_id == 0 ? 0 : 123 * kMs;
        event.output_path =
            request.kind == olouie::record::VideoRecorderExportKind::Bookmark
                ? L"saved-bookmark.mp4"
                : L"saved-clip.mp4";
        event.message = event.state == olouie::record::VideoRecorderClipState::Saved
                            ? L"Recording export saved."
                            : L"injected recording export failure";
        options.clip_event_sink(event);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sink(VideoRecorderPipelineStage::CaptureStop);
    VideoRecorderPipelineResult result;
    result.status = VideoRecorderPipelineStatus::Success;
    result.failed_stage = VideoRecorderPipelineStage::Complete;
    result.session_directory = L"session";
    result.output_path = L"saved.mp4";
    result.stats.encoded_packet_count = 4;
    return result;
  };

  olouie::record::VideoRecorderSession session(options, runner);
  if (session.StopAndSave().Accepted()) {
    return Fail("Idle video recorder should reject stop-and-save.");
  }
  if (session.SaveLastClip(std::chrono::seconds(30)).Accepted()) {
    return Fail("Idle video recorder should reject clip requests.");
  }
  if (session.AddBookmarkAndSave(std::chrono::seconds(60),
                                 std::chrono::milliseconds(0))
          .Accepted()) {
    return Fail("Idle video recorder should reject bookmark requests.");
  }
  if (!session.Start().Accepted() ||
      !WaitForRecorderState(&session, VideoRecorderState::Recording)) {
    return Fail("Video recorder session did not enter Recording.");
  }
  if (session.Start().Accepted()) {
    return Fail("Video recorder session should reject repeated start.");
  }
  const auto active = session.Snapshot();
  if (active.session_directory != L"active-session" ||
      active.stats.captured_frame_count != 60 ||
      active.diagnostics_generation == 0) {
    return Fail("Recorder progress should publish copied live diagnostics.");
  }
  if (!session.SaveLastClip(std::chrono::minutes(5)).Accepted()) {
    return Fail("Recording session should accept the 5-minute clip preset.");
  }
  const auto clip_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < clip_deadline &&
         session.Snapshot().clip.state !=
             olouie::record::VideoRecorderClipState::Saved) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto clipped = session.Snapshot();
  if (clipped.state != VideoRecorderState::Recording ||
      clipped.clip.state != olouie::record::VideoRecorderClipState::Saved ||
      clipped.clip.kind != olouie::record::VideoRecorderExportKind::Clip ||
      clipped.clip.duration != std::chrono::minutes(5) ||
      clipped.clip.output_path != L"saved-clip.mp4" ||
      clipped.clip_event_generation < 2) {
    return Fail("Clip completion should leave the recorder in Recording.");
  }

  if (!session.AddBookmarkAndSave(std::chrono::seconds(60),
                                  std::chrono::milliseconds(0))
           .Accepted()) {
    return Fail("Recording session should accept an active bookmark request.");
  }
  const auto bookmark_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < bookmark_deadline &&
         (session.Snapshot().clip.state !=
              olouie::record::VideoRecorderClipState::Saved ||
          session.Snapshot().clip.kind !=
              olouie::record::VideoRecorderExportKind::Bookmark)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto bookmarked = session.Snapshot();
  if (bookmarked.state != VideoRecorderState::Recording ||
      bookmarked.clip.kind !=
          olouie::record::VideoRecorderExportKind::Bookmark ||
      bookmarked.clip.state !=
          olouie::record::VideoRecorderClipState::Saved ||
      bookmarked.clip.bookmark_id != 7 ||
      bookmarked.clip.bookmark_time_ns != 123 * kMs ||
      bookmarked.clip.output_path != L"saved-bookmark.mp4") {
    return Fail("Bookmark completion should preserve marker data and Recording.");
  }

  if (!session.AddBookmarkAndSave(std::chrono::seconds(61),
                                  std::chrono::milliseconds(0))
           .Accepted()) {
    return Fail("Recording session should accept repeated bookmark requests.");
  }
  const auto bookmark_failure_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < bookmark_failure_deadline &&
         session.Snapshot().clip.state !=
             olouie::record::VideoRecorderClipState::Failed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto bookmark_failed = session.Snapshot();
  if (bookmark_failed.state != VideoRecorderState::Recording ||
      bookmark_failed.clip.kind !=
          olouie::record::VideoRecorderExportKind::Bookmark ||
      bookmark_failed.clip.state !=
          olouie::record::VideoRecorderClipState::Failed ||
      bookmark_failed.clip.bookmark_id != 7 ||
      bookmark_failed.clip.message.find(L"injected") == std::wstring::npos) {
    return Fail("Bookmark export failure should not stop active recording.");
  }

  if (!session.SaveLastClip(std::chrono::seconds(31)).Accepted()) {
    return Fail("Recording session should accept a repeated clip request.");
  }
  const auto failure_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < failure_deadline &&
         session.Snapshot().clip.state !=
             olouie::record::VideoRecorderClipState::Failed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto clip_failed = session.Snapshot();
  if (clip_failed.state != VideoRecorderState::Recording ||
      clip_failed.clip.state != olouie::record::VideoRecorderClipState::Failed ||
      clip_failed.clip.message.find(L"injected") == std::wstring::npos) {
    return Fail("Clip export failure should not stop the active recording.");
  }
  if (!session.StopAndSave().Accepted() ||
      session.StopAndSave().Accepted() ||
      !WaitForRecorderState(&session, VideoRecorderState::Saved)) {
    return Fail("Video recorder stop/save state transition is incorrect.");
  }
  const auto saved = session.Snapshot();
  if (saved.output_path != L"saved.mp4" ||
      saved.stats.encoded_packet_count != 4) {
    return Fail("Video recorder saved result was not retained.");
  }
  session.Shutdown();

  auto failing_runner = [](const olouie::record::VideoRecorderPipelineOptions&,
                           std::atomic_bool*,
                           olouie::record::VideoRecorderPipelineStageSink) {
    VideoRecorderPipelineResult result;
    result.status = VideoRecorderPipelineStatus::PrepareFailed;
    result.failed_stage = VideoRecorderPipelineStage::Prepare;
    result.message = L"injected prepare failure";
    return result;
  };
  olouie::record::VideoRecorderSession failing_session(options,
                                                       failing_runner);
  if (!failing_session.Start().Accepted() ||
      !WaitForRecorderState(&failing_session, VideoRecorderState::Failed) ||
      failing_session.Snapshot().message.find(L"injected prepare failure") ==
          std::wstring::npos) {
    return Fail("Video recorder session should preserve pipeline failures.");
  }
  failing_session.Shutdown();

  auto preserved_failure_runner = [](
                                      const olouie::record::VideoRecorderPipelineOptions&,
                                      std::atomic_bool*,
                                      olouie::record::VideoRecorderPipelineStageSink) {
    VideoRecorderPipelineResult result;
    result.status = VideoRecorderPipelineStatus::CaptureFailed;
    result.failed_stage = VideoRecorderPipelineStage::Recording;
    result.message = L"selected monitor disconnected; saved recovered.mp4";
    result.output_path = L"recovered.mp4";
    result.recording_saved_after_failure = true;
    return result;
  };
  olouie::record::VideoRecorderSession preserved_failure_session(
      options, preserved_failure_runner);
  if (!preserved_failure_session.Start().Accepted() ||
      !WaitForRecorderState(&preserved_failure_session,
                            VideoRecorderState::Failed)) {
    return Fail("Saved topology failure did not reach the failed recorder state.");
  }
  const auto preserved_failure = preserved_failure_session.Snapshot();
  if (!preserved_failure.recording_saved_after_failure ||
      preserved_failure.output_path != L"recovered.mp4" ||
      preserved_failure.message.find(L"Recording stopped during recording") ==
          std::wstring::npos ||
      preserved_failure.message.find(L"selected monitor disconnected") ==
          std::wstring::npos) {
    return Fail("Recorder state should report the path saved after topology failure.");
  }
  preserved_failure_session.Shutdown();

  auto cancellable_runner = [](
                                const olouie::record::VideoRecorderPipelineOptions&,
                                std::atomic_bool* stop_requested,
                                olouie::record::VideoRecorderPipelineStageSink) {
    while (!stop_requested->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VideoRecorderPipelineResult result;
    result.status = VideoRecorderPipelineStatus::Cancelled;
    result.failed_stage = VideoRecorderPipelineStage::Prepare;
    result.message = L"cancelled during start";
    return result;
  };
  olouie::record::VideoRecorderSession cancellable_session(
      options, cancellable_runner);
  if (!cancellable_session.Start().Accepted() ||
      !cancellable_session.StopAndSave().Accepted() ||
      !WaitForRecorderState(&cancellable_session, VideoRecorderState::Idle) ||
      cancellable_session.Snapshot().message != L"cancelled during start") {
    return Fail("Video recorder start cancellation state is incorrect.");
  }
  cancellable_session.Shutdown();
  return 0;
}

int TestDiagnosticsSnapshot() {
  olouie::settings::AppSettings settings;
  settings.output_directory = L"C:\\O'Louie\\Videos";
  settings.video.fps = 60;
  settings.video.bitrate_mbps = 20;
  settings.audio.system_mix = true;
  settings.audio.mic = true;

  olouie::record::VideoRecorderSnapshot recorder;
  olouie::record::RecordingRecoverySnapshot recovery;
  auto idle = olouie::diagnostics::BuildDiagnosticsSnapshot(
      settings, recorder, recovery);
  if (idle.recorder_state != olouie::record::VideoRecorderState::Idle ||
      idle.monitor_identity.find(L"Primary") == std::wstring::npos ||
      idle.requested_fps != 60.0 ||
      idle.requested_bitrate_bps != 20000000ULL ||
      idle.audio_tracks.size() != 2 ||
      !idle.first_actionable_failure.empty()) {
    return Fail("Idle diagnostics should expose configured fallback facts.");
  }

  recorder.state = olouie::record::VideoRecorderState::Recording;
  recorder.session_directory = L"C:\\O'Louie\\Sessions\\active";
  recorder.output_path = L"C:\\O'Louie\\Videos\\active.mp4";
  recorder.stats.captured_frame_count = 130;
  recorder.stats.accepted_frame_count = 118;
  recorder.stats.rate_limited_frame_count = 10;
  recorder.stats.dropped_frame_count = 2;
  recorder.stats.encoded_frame_count = 118;
  recorder.stats.runtime.monitor_device_name = L"DISPLAY2";
  recorder.stats.runtime.encoder_name = L"Hardware H.264 Test MFT";
  recorder.stats.runtime.requested_fps_numerator = 60;
  recorder.stats.runtime.requested_fps_denominator = 1;
  recorder.stats.runtime.requested_bitrate_bps = 20000000;
  recorder.stats.runtime.negotiated_fps_numerator = 60;
  recorder.stats.runtime.negotiated_fps_denominator = 1;
  recorder.stats.runtime.negotiated_bitrate_bps = 20000000;
  recorder.stats.runtime.recording_elapsed_ns = 2000000000ULL;
  recorder.stats.runtime.encoded_video_payload_bytes = 5000000ULL;
  recorder.stats.runtime.queued_video_frame_count = 2;
  recorder.stats.runtime.peak_queued_video_frame_count = 8;
  recorder.stats.runtime.video_queue_capacity = 8;
  recorder.stats.runtime.video_queue_oldest_frame_age_ns = 33000000;
  recorder.stats.runtime.video_queue_maximum_frame_age_ns = 133000000;
  recorder.stats.runtime.video_queue_overflow_event_count = 1;
  recorder.stats.runtime.video_queue_backlog_recovery_count = 1;
  recorder.stats.runtime.video_queue_dropped_backlog_count = 8;
  recorder.stats.runtime.video_queue_last_overflow_reason =
      olouie::capture::VideoFrameQueueOverflowReason::BacklogDiscarded;
  recorder.stats.runtime.video_texture_pool.capacity = 10;
  recorder.stats.runtime.video_texture_pool.allocated_texture_count = 10;
  recorder.stats.runtime.video_texture_pool.in_use_texture_count = 3;
  recorder.stats.runtime.video_texture_pool.peak_in_use_texture_count = 10;
  recorder.stats.runtime.video_texture_pool.created_texture_count = 10;
  recorder.stats.runtime.video_texture_pool.reused_texture_count = 108;
  recorder.stats.runtime.video_texture_pool_exhausted_frame_count = 2;
  recorder.stats.runtime.video_converter.input_view_create_count = 10;
  recorder.stats.runtime.video_converter.input_view_reuse_count = 108;
  recorder.stats.runtime.video_converter.output_view_create_count = 1;
  recorder.stats.runtime.video_converter.output_view_reuse_count = 117;
  recorder.stats.runtime.video_capture_copy_submission_count = 2;
  recorder.stats.runtime.video_capture_copy_last_latency_ns = 100;
  recorder.stats.runtime.video_capture_copy_maximum_latency_ns = 150;
  recorder.stats.runtime.video_capture_copy_total_latency_ns = 250;
  recorder.stats.runtime.video_queue_wait_sample_count = 2;
  recorder.stats.runtime.video_queue_wait_last_ns = 200;
  recorder.stats.runtime.video_queue_wait_maximum_ns = 300;
  recorder.stats.runtime.video_queue_wait_total_ns = 500;
  recorder.stats.runtime.video_converter.conversion_submission_count = 2;
  recorder.stats.runtime.video_converter
      .last_conversion_submission_latency_ns = 400;
  recorder.stats.runtime.video_converter
      .maximum_conversion_submission_latency_ns = 500;
  recorder.stats.runtime.video_converter
      .total_conversion_submission_latency_ns = 900;
  recorder.stats.runtime.video_encoder_wait_count = 2;
  recorder.stats.runtime.video_encoder_wait_last_ns = 600;
  recorder.stats.runtime.video_encoder_wait_maximum_ns = 700;
  recorder.stats.runtime.video_encoder_wait_total_ns = 1300;
  recorder.stats.runtime.performance_mode =
      olouie::performance::CapturePerformanceMode::CaptureFirst;
  recorder.stats.runtime.process_priority_class = NORMAL_PRIORITY_CLASS;
  recorder.stats.runtime.recorder_scheduling.registered = true;
  recorder.stats.runtime.recorder_scheduling.priority_applied = true;
  recorder.stats.runtime.capture_scheduling.registered = true;
  recorder.stats.runtime.capture_scheduling.priority_applied = true;
  recorder.stats.runtime.video_encode_scheduling.registered = true;
  recorder.stats.runtime.video_encode_scheduling.priority_applied = true;
  olouie::performance::MultimediaThreadSchedulingSnapshot audio_scheduling;
  audio_scheduling.attempted = true;
  audio_scheduling.registered = true;
  audio_scheduling.priority_applied = true;
  audio_scheduling.plan =
      olouie::performance::BuildMultimediaThreadSchedulingPlan(
          olouie::performance::CapturePerformanceMode::CaptureFirst,
          olouie::performance::MultimediaThreadWorkload::AudioCapture);
  recorder.stats.runtime.audio_capture_scheduling.push_back(
      audio_scheduling);
  olouie::encode::MfHardwareH264CodecSettingResult low_latency;
  low_latency.name = L"low latency mode";
  low_latency.requested_value = 1;
  low_latency.supported = true;
  low_latency.modifiable = true;
  low_latency.attempted = true;
  low_latency.applied = true;
  low_latency.read_back = true;
  low_latency.accepted_value = 1;
  low_latency.message = L"Applied and confirmed.";
  recorder.stats.runtime.encoder_codec_settings.push_back(low_latency);
  recorder.stats.runtime.packet_writer.persisted_packet_count = 2;
  recorder.stats.runtime.packet_writer.peak_queued_packet_count = 3;
  recorder.stats.runtime.packet_writer.last_write_latency_ns = 800;
  recorder.stats.runtime.packet_writer.maximum_write_latency_ns = 900;
  recorder.stats.runtime.packet_writer.total_write_latency_ns = 1700;
  recorder.stats.runtime.outstanding_export_count = 1;
  recorder.stats.runtime.audio_tracks = {
      {2, L"System audio", true}, {3, L"Microphone", false}};
  recovery.state = olouie::record::RecordingRecoveryState::Ready;
  auto active = olouie::diagnostics::BuildDiagnosticsSnapshot(
      settings, recorder, recovery);
  if (active.monitor_identity != L"DISPLAY2" ||
      active.encoder_identity != L"Hardware H.264 Test MFT" ||
      active.observed_fps != 59.0 ||
      active.observed_bitrate_bps != 20000000ULL ||
      active.audio_tracks.size() != 2 ||
      !active.audio_tracks.front().packet_bearing ||
      active.recorder_stats.rate_limited_frame_count != 10 ||
      active.recorder_stats.runtime.outstanding_export_count != 1 ||
      active.recorder_stats.runtime.video_queue_dropped_backlog_count != 8 ||
      active.recorder_stats.runtime.video_texture_pool.reused_texture_count !=
          108 ||
      active.recorder_stats.runtime.video_converter.output_view_reuse_count !=
          117) {
    return Fail("Active diagnostics should calculate observed recorder facts.");
  }

  recorder.state = olouie::record::VideoRecorderState::Saved;
  recorder.message = L"Recording saved.";
  auto saved = olouie::diagnostics::BuildDiagnosticsSnapshot(
      settings, recorder, recovery);
  if (saved.recorder_state != olouie::record::VideoRecorderState::Saved ||
      saved.recording_output_path != L"C:\\O'Louie\\Videos\\active.mp4" ||
      !saved.first_actionable_failure.empty()) {
    return Fail("Saved diagnostics should retain the output without a failure.");
  }

  recorder.state = olouie::record::VideoRecorderState::Failed;
  recorder.message = L"Hardware encoder failed first.";
  recorder.clip.state = olouie::record::VideoRecorderClipState::Failed;
  recorder.clip.message = L"Secondary export failure.";
  auto failed = olouie::diagnostics::BuildDiagnosticsSnapshot(
      settings, recorder, recovery);
  const auto report = olouie::diagnostics::FormatDiagnosticsReport(failed);
  if (failed.first_actionable_failure != L"Hardware encoder failed first." ||
      report.find(L"Hardware encoder failed first.") == std::wstring::npos ||
      report.find(L"active.mp4") == std::wstring::npos ||
      report.find(L"Video queue current / peak / capacity: 2 / 8 / 8") ==
          std::wstring::npos ||
      report.find(L"stale backlog discarded") == std::wstring::npos ||
      report.find(L"Capture texture pool created / reused / exhausted frames: "
                  L"10 / 108 / 2") == std::wstring::npos ||
      report.find(L"Converter output views created / reused: 1 / 117") ==
          std::wstring::npos ||
      report.find(L"Capture copy submission latency last / maximum / average "
                  L"ns: 100 / 150 / 125") == std::wstring::npos ||
      report.find(L"Packet write latency last / maximum / average ns: 800 / "
                  L"900 / 850") ==
          std::wstring::npos ||
      report.find(L"Recording performance mode: capture first") ==
          std::wstring::npos ||
      report.find(L"Process priority class / real-time: 32 / no") ==
          std::wstring::npos ||
      report.find(L"Video encode MMCSS registered / priority applied: yes / "
                  L"yes") == std::wstring::npos ||
      report.find(L"Audio capture MMCSS applied / sources: 1 / 1") ==
          std::wstring::npos ||
      report.find(L"low latency mode: requested=1, supported=yes, "
                  L"modifiable=yes, applied=yes, accepted=1") ==
          std::wstring::npos) {
    return Fail("Diagnostics should preserve the first actionable failure and paths.");
  }

  recorder = {};
  recovery.state = olouie::record::RecordingRecoveryState::Failed;
  recovery.message = L"Recovery scan could not read a session.";
  auto recovery_failed = olouie::diagnostics::BuildDiagnosticsSnapshot(
      settings, recorder, recovery);
  if (recovery_failed.first_actionable_failure != recovery.message) {
    return Fail("Recovery failures should surface in diagnostics.");
  }
  return 0;
}

int TestVideoRecorderNoFfmpegBoundary() {
#if !OLOUIE_FFMPEG_CONFIGURED
  olouie::record::VideoRecorderPipelineOptions options;
  options.drain_interval = std::chrono::milliseconds(1);
  std::atomic_bool stop_requested{false};
  const auto result = olouie::record::RunVideoRecorderPipeline(
      options, &stop_requested);
  if (result.status !=
      olouie::record::VideoRecorderPipelineStatus::BackendUnavailable) {
    return Fail("Default recorder pipeline should fail before capture when "
                "FFmpeg is unavailable.");
  }
#endif
  return 0;
}

}  // namespace

int main() {
  if (const int result = TestTimebase(); result != 0) {
    return result;
  }

  if (const int result = TestSessionClock(); result != 0) {
    return result;
  }

  if (const int result = TestPacketStore(); result != 0) {
    return result;
  }

  if (const int result = TestAsynchronousPacketWriter(); result != 0) {
    return result;
  }

  if (const int result = TestDiskWriteFailures(); result != 0) {
    return result;
  }

  if (const int result = TestBookmarks(); result != 0) {
    return result;
  }

  if (const int result = TestSessionManifest(); result != 0) {
    return result;
  }

  if (const int result = TestVideoExportPlan(); result != 0) {
    return result;
  }

  if (const int result = TestActiveRecordingClip(); result != 0) {
    return result;
  }

  if (const int result = TestRecordingRecovery(); result != 0) {
    return result;
  }

  if (const int result = TestVideoRecorderPipelineOrdering(); result != 0) {
    return result;
  }

  if (const int result = TestVideoRecorderSessionState(); result != 0) {
    return result;
  }

  if (const int result = TestDiagnosticsSnapshot(); result != 0) {
    return result;
  }

  if (const int result = TestVideoRecorderNoFfmpegBoundary(); result != 0) {
    return result;
  }

  return 0;
}
