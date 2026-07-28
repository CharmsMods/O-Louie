#include <algorithm>
#include <cstddef>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <winrt/base.h>

#include "encode/H264PacketStore.h"
#include "encode/MfHardwareH264EncoderProbe.h"
#include "encode/MfHardwareH264EncoderSession.h"
#include "encode/VideoRecordingBootstrap.h"
#include "encode/VideoEncodeThread.h"
#include "encode/VideoRecordingMetadata.h"
#include "encode/VideoRecordingRunSession.h"
#include "encode/VideoRecordingSetup.h"
#include "encode/VideoRecordingSession.h"
#include "graphics/D3D11DeviceFault.h"
#include "performance/MultimediaThreadScheduling.h"
#include "record/PacketStore.h"

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

int VerifyCapturePerformancePolicy() {
  using olouie::performance::CapturePerformanceMode;
  using olouie::performance::MultimediaThreadRelativePriority;
  using olouie::performance::MultimediaThreadWorkload;

  const auto balanced =
      olouie::encode::BuildMfHardwareH264PerformanceTuningPlan(
          CapturePerformanceMode::Balanced);
  const auto capture_first =
      olouie::encode::BuildMfHardwareH264PerformanceTuningPlan(
          CapturePerformanceMode::CaptureFirst);
  if (!balanced.IsValid() || balanced.apply_low_latency ||
      balanced.apply_quality_vs_speed || !capture_first.IsValid() ||
      !capture_first.apply_low_latency ||
      !capture_first.requested_low_latency ||
      !capture_first.apply_quality_vs_speed ||
      capture_first.requested_quality_vs_speed != 0) {
    return Fail("Hardware encoder performance tuning plans are incorrect.");
  }

  const auto balanced_schedule =
      olouie::performance::BuildMultimediaThreadSchedulingPlan(
          CapturePerformanceMode::Balanced,
          MultimediaThreadWorkload::Recorder);
  const auto capture_first_schedule =
      olouie::performance::BuildMultimediaThreadSchedulingPlan(
          CapturePerformanceMode::CaptureFirst,
          MultimediaThreadWorkload::VideoEncode);
  const auto audio_schedule =
      olouie::performance::BuildMultimediaThreadSchedulingPlan(
          CapturePerformanceMode::CaptureFirst,
          MultimediaThreadWorkload::AudioCapture);
  if (!balanced_schedule.IsValid() || !capture_first_schedule.IsValid() ||
      balanced_schedule.task_name != L"Capture" ||
      balanced_schedule.relative_priority !=
          MultimediaThreadRelativePriority::Normal ||
      capture_first_schedule.relative_priority !=
          MultimediaThreadRelativePriority::High ||
      audio_schedule.task_name != L"Audio" || !audio_schedule.IsValid() ||
      audio_schedule.relative_priority !=
          MultimediaThreadRelativePriority::High ||
      balanced_schedule.changes_process_priority ||
      capture_first_schedule.changes_process_priority) {
    return Fail("MMCSS scheduling plans are not conservative and thread-scoped.");
  }
  auto invalid_schedule = balanced_schedule;
  invalid_schedule.mode = static_cast<CapturePerformanceMode>(99);
  if (invalid_schedule.IsValid()) {
    return Fail("MMCSS scheduling should reject invalid performance modes.");
  }

  olouie::performance::MultimediaThreadRegistration registration;
  const auto scheduling = registration.Register(balanced_schedule);
  if (!scheduling.attempted ||
      scheduling.plan.workload != MultimediaThreadWorkload::Recorder) {
    return Fail("MMCSS scheduling attempt telemetry is incomplete.");
  }
  registration.Reset();
  if (registration.snapshot().attempted) {
    return Fail("MMCSS thread registration did not reset cleanly.");
  }
  const auto audio_scheduling = registration.Register(audio_schedule);
  if (!audio_scheduling.attempted ||
      audio_scheduling.plan.workload !=
          MultimediaThreadWorkload::AudioCapture ||
      audio_scheduling.plan.task_name != L"Audio") {
    return Fail("Audio MMCSS scheduling attempt telemetry is incomplete.");
  }
  registration.Reset();
  return 0;
}

bool BytesEqual(const std::vector<std::byte>& left,
                const std::vector<uint8_t>& right) {
  if (left.size() != right.size()) {
    return false;
  }

  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index] != static_cast<std::byte>(right[index])) {
      return false;
    }
  }
  return true;
}

struct FakeEnumeratorState {
  uint32_t call_count = 0;
  olouie::encode::MfHardwareH264EncoderProbeOptions last_options;
  olouie::encode::MfHardwareH264EncoderProbeStatus status =
      olouie::encode::MfHardwareH264EncoderProbeStatus::Success;
  std::vector<olouie::encode::MfHardwareH264EncoderInfo> encoders;
  std::wstring message;
};

FakeEnumeratorState g_fake_enumerator_state;

void ResetFakeEnumeratorState() {
  g_fake_enumerator_state = {};
  g_fake_enumerator_state.encoders = {
      olouie::encode::MfHardwareH264EncoderInfo{
          L"Fake Hardware H.264 Encoder A", L"{00000000-0000-0000-0000-000000000001}", 7},
      olouie::encode::MfHardwareH264EncoderInfo{
          L"Fake Hardware H.264 Encoder B", L"{00000000-0000-0000-0000-000000000002}", 7},
  };
}

olouie::encode::MfHardwareH264EncoderProbeStatus FakeEnumerator(
    const olouie::encode::MfHardwareH264EncoderProbeOptions& options,
    std::vector<olouie::encode::MfHardwareH264EncoderInfo>* encoders,
    std::wstring* message) {
  ++g_fake_enumerator_state.call_count;
  g_fake_enumerator_state.last_options = options;

  if (message != nullptr) {
    *message = g_fake_enumerator_state.message;
  }
  if (encoders != nullptr) {
    *encoders = g_fake_enumerator_state.encoders;
  }
  return g_fake_enumerator_state.status;
}

int VerifyMfHardwareH264EncoderConfigValidation() {
  olouie::encode::MfHardwareH264EncoderConfig config;
  auto result = olouie::encode::ValidateMfHardwareH264EncoderConfig(config);
  if (!result.Succeeded()) {
    return Fail("Valid H.264 encoder config should pass validation.");
  }

  auto invalid = config;
  invalid.width = 0;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject zero width.");
  }

  invalid = config;
  invalid.height = 1079;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject odd dimensions.");
  }

  invalid = config;
  invalid.fps_numerator = 0;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject zero FPS numerator.");
  }

  invalid = config;
  invalid.fps_denominator = 0;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject zero FPS denominator.");
  }

  invalid = config;
  invalid.bitrate_bps = 0;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject zero bitrate.");
  }

  invalid = config;
  invalid.gop_seconds = 0.0;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject non-positive GOP.");
  }

  invalid = config;
  invalid.max_b_frames = 2;
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should keep B-frames deferred.");
  }

  invalid = config;
  invalid.performance_mode =
      static_cast<olouie::performance::CapturePerformanceMode>(99);
  if (olouie::encode::ValidateMfHardwareH264EncoderConfig(invalid).status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 validation should reject an invalid performance mode.");
  }

  return 0;
}

int VerifyMfHardwareH264EncoderProbeBoundary() {
  olouie::encode::MfHardwareH264EncoderConfig config;
  olouie::encode::MfHardwareH264EncoderProbeOptions options;
  options.include_local_mfts = false;

  ResetFakeEnumeratorState();
  auto result = olouie::encode::ProbeMfHardwareH264Encoder(
      config, options, &FakeEnumerator);
  if (!result.Succeeded() || result.encoders.size() != 2 ||
      result.selected_encoder_index != 0 ||
      result.selected_encoder() == nullptr ||
      result.selected_encoder()->name != L"Fake Hardware H.264 Encoder A" ||
      result.options.include_local_mfts ||
      g_fake_enumerator_state.call_count != 1 ||
      g_fake_enumerator_state.last_options.include_local_mfts) {
    return Fail("H.264 probe should select the first hardware encoder.");
  }

  ResetFakeEnumeratorState();
  g_fake_enumerator_state.encoders.clear();
  result = olouie::encode::ProbeMfHardwareH264Encoder(
      config, options, &FakeEnumerator);
  if (result.status !=
          olouie::encode::MfHardwareH264EncoderProbeStatus::
              HardwareEncoderUnavailable ||
      result.selected_encoder() != nullptr ||
      g_fake_enumerator_state.call_count != 1) {
    return Fail("H.264 probe should fail explicitly without hardware encoders.");
  }

  ResetFakeEnumeratorState();
  g_fake_enumerator_state.status =
      olouie::encode::MfHardwareH264EncoderProbeStatus::EnumerationFailed;
  g_fake_enumerator_state.message = L"fake enumeration failure";
  result = olouie::encode::ProbeMfHardwareH264Encoder(
      config, options, &FakeEnumerator);
  if (result.status !=
          olouie::encode::MfHardwareH264EncoderProbeStatus::
              EnumerationFailed ||
      result.message != L"fake enumeration failure") {
    return Fail("H.264 probe should surface enumeration failures.");
  }

  ResetFakeEnumeratorState();
  config.width = 1919;
  result = olouie::encode::ProbeMfHardwareH264Encoder(
      config, options, &FakeEnumerator);
  if (result.status !=
          olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig ||
      g_fake_enumerator_state.call_count != 0) {
    return Fail("H.264 probe should validate before enumeration.");
  }

  if (olouie::encode::ProbeMfHardwareH264Encoder(
          olouie::encode::MfHardwareH264EncoderConfig{}, options, nullptr)
          .status !=
      olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig) {
    return Fail("H.264 probe should reject a missing enumerator.");
  }

  if (std::wstring(olouie::encode::MfHardwareH264EncoderProbeStatusName(
          olouie::encode::MfHardwareH264EncoderProbeStatus::
              HardwareEncoderUnavailable)) !=
      L"hardware encoder unavailable") {
    return Fail("H.264 probe status names changed unexpectedly.");
  }

  return 0;
}

int VerifyMfHardwareH264EncoderSessionPlan() {
  olouie::encode::MfHardwareH264EncoderConfig config;
  config.width = 2560;
  config.height = 1440;
  config.fps_numerator = 60;
  config.fps_denominator = 1;
  config.bitrate_bps = 40000000;
  config.gop_seconds = 2.0;
  config.max_b_frames = 0;

  olouie::encode::MfHardwareH264EncoderMediaTypePlan plan;
  auto result =
      olouie::encode::BuildMfHardwareH264EncoderMediaTypePlan(config, &plan);
  if (!result.Succeeded() || !plan.IsValid() || plan.width != config.width ||
      plan.height != config.height ||
      plan.fps_numerator != config.fps_numerator ||
      plan.fps_denominator != config.fps_denominator ||
      plan.bitrate_bps != config.bitrate_bps ||
      plan.gop_frame_count != 120 || plan.h264_profile != 100 ||
      plan.max_b_frames != 0) {
    return Fail("H.264 session media type plan is incorrect.");
  }

  config.fps_numerator = 30000;
  config.fps_denominator = 1001;
  config.gop_seconds = 2.0;
  result =
      olouie::encode::BuildMfHardwareH264EncoderMediaTypePlan(config, &plan);
  if (!result.Succeeded() || plan.gop_frame_count != 60) {
    return Fail("H.264 session should round fractional GOP frame counts.");
  }

  if (olouie::encode::BuildMfHardwareH264EncoderMediaTypePlan(config, nullptr)
          .status !=
      olouie::encode::MfHardwareH264EncoderSessionStatus::InvalidConfig) {
    return Fail("H.264 session plan should reject missing output.");
  }

  config.width = 2559;
  if (olouie::encode::BuildMfHardwareH264EncoderMediaTypePlan(config, &plan)
          .status !=
      olouie::encode::MfHardwareH264EncoderSessionStatus::InvalidConfig) {
    return Fail("H.264 session plan should surface invalid configs.");
  }

  if (std::wstring(olouie::encode::MfHardwareH264EncoderSessionStatusName(
          olouie::encode::MfHardwareH264EncoderSessionStatus::
              OutputTypeRejected)) != L"output type rejected") {
    return Fail("H.264 session status names changed unexpectedly.");
  }

  if (std::wstring(olouie::encode::MfHardwareH264EncoderSessionStatusName(
          olouie::encode::MfHardwareH264EncoderSessionStatus::
              DeviceManagerAttachFailed)) !=
      L"device manager attach failed") {
    return Fail("H.264 device-manager status names changed unexpectedly.");
  }

  const olouie::encode::MfHardwareH264EncoderSessionInfo info;
  if (info.d3d11_device_supplied || info.device_manager_created ||
      info.device_manager_reset || info.device_manager_attached ||
      info.device_manager_reset_token != 0 ||
      info.synthetic_input_submitted || info.submitted_input_frames != 0 ||
      info.pending_input_samples != 0) {
    return Fail("H.264 session device-manager info should default inactive.");
  }

  if (std::wstring(
          olouie::encode::MfHardwareH264EncoderFrameSubmitStatusName(
              olouie::encode::MfHardwareH264EncoderFrameSubmitStatus::
                  ProcessInputFailed)) != L"process input failed") {
    return Fail("H.264 frame-submit status names changed unexpectedly.");
  }

  const olouie::encode::MfHardwareH264EncoderFrameSubmitResult submit_result;
  if (submit_result.Succeeded()) {
    return Fail("Default H.264 frame-submit result should not succeed.");
  }

  olouie::encode::MfHardwareH264EncoderSession session;
  if (session.pending_input_sample_count() != 0) {
    return Fail("H.264 session should not retain samples before input.");
  }
  if (session.SubmitSyntheticNv12Frame(nullptr, 0, 1).status !=
      olouie::encode::MfHardwareH264EncoderFrameSubmitStatus::NotConfigured) {
    return Fail("H.264 synthetic frame submit should require configuration.");
  }
  if (session.SubmitNv12Texture(nullptr, 0, 1).status !=
      olouie::encode::MfHardwareH264EncoderFrameSubmitStatus::NotConfigured) {
    return Fail("H.264 caller-owned texture submit should require configuration.");
  }

  if (std::wstring(olouie::encode::MfHardwareH264EncoderDrainStatusName(
          olouie::encode::MfHardwareH264EncoderDrainStatus::
              ProcessOutputFailed)) != L"process output failed") {
    return Fail("H.264 drain status names changed unexpectedly.");
  }

  const olouie::encode::MfHardwareH264EncoderDrainResult drain_result;
  if (drain_result.Succeeded() || !drain_result.packets.empty() ||
      drain_result.events_checked != 0 || drain_result.drain_command_sent ||
      drain_result.saw_have_output_event || drain_result.saw_need_input_event) {
    return Fail("Default H.264 drain result should be inactive.");
  }

  if (session.DrainSyntheticEncodedOutput(1).status !=
      olouie::encode::MfHardwareH264EncoderDrainStatus::NotConfigured) {
    return Fail("H.264 synthetic output drain should require configuration.");
  }
  session.Reset();
  if (session.pending_input_sample_count() != 0 ||
      session.info().pending_input_samples != 0) {
    return Fail("H.264 session reset should release retained input samples.");
  }

  return 0;
}

int VerifyMfHardwareH264BitstreamInspection() {
  const std::vector<uint8_t> annex_b = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
      0x00, 0x00, 0x01,       0x68, 0xce, 0x06,
      0x00, 0x00, 0x01,       0x65, 0x88, 0x84,
  };

  const auto info = olouie::encode::InspectMfHardwareH264Bitstream(annex_b);
  if (info.packet_format != olouie::encode::MfHardwareH264PacketFormat::AnnexB ||
      info.nal_unit_count != 3 || info.sps_count != 1 ||
      info.pps_count != 1 || info.idr_count != 1 || !info.has_sps ||
      !info.has_pps || !info.has_idr || !info.mp4_extradata_ready) {
    return Fail("H.264 Annex B bitstream inspection is incorrect.");
  }
  const std::vector<uint8_t> expected_sps = {0x67, 0x42, 0x00, 0x1f};
  const std::vector<uint8_t> expected_pps = {0x68, 0xce, 0x06};
  const std::vector<uint8_t> expected_avcc = {
      0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67,
      0x42, 0x00, 0x1f, 0x01, 0x00, 0x03, 0x68, 0xce, 0x06,
  };
  if (!info.config.IsReady() || info.config.sps != expected_sps ||
      info.config.pps != expected_pps ||
      !info.config.HasAvccExtradata() ||
      info.config.avcc_extradata != expected_avcc ||
      info.config.packet_format !=
          olouie::encode::MfHardwareH264PacketFormat::AnnexB) {
    return Fail("H.264 config record should capture SPS/PPS and AVCC bytes.");
  }

  const std::vector<uint8_t> no_config = {0x00, 0x00, 0x01, 0x65, 0x01};
  const auto no_config_info =
      olouie::encode::InspectMfHardwareH264Bitstream(no_config);
  if (no_config_info.packet_format !=
          olouie::encode::MfHardwareH264PacketFormat::AnnexB ||
      no_config_info.nal_unit_count != 1 || no_config_info.has_sps ||
      no_config_info.has_pps || !no_config_info.has_idr ||
      no_config_info.mp4_extradata_ready || no_config_info.config.IsReady()) {
    return Fail("H.264 bitstream inspection should require SPS and PPS.");
  }

  const olouie::encode::MfHardwareH264ConfigRecord empty_config;
  const auto empty_avcc =
      olouie::encode::BuildMfHardwareH264AvccExtradata(empty_config);
  if (empty_config.IsReady() || empty_config.HasAvccExtradata() ||
      empty_avcc.IsValid()) {
    return Fail("Default H.264 config record should not be ready.");
  }

  olouie::encode::MfHardwareH264ConfigRecord short_sps_config;
  short_sps_config.packet_format =
      olouie::encode::MfHardwareH264PacketFormat::AnnexB;
  short_sps_config.sps = {0x67, 0x42, 0x00};
  short_sps_config.pps = expected_pps;
  if (olouie::encode::BuildMfHardwareH264AvccExtradata(short_sps_config)
          .IsValid()) {
    return Fail("AVCC builder should reject SPS payloads without profile data.");
  }

  if (std::wstring(olouie::encode::MfHardwareH264PacketFormatName(
          olouie::encode::MfHardwareH264PacketFormat::AnnexB)) !=
      L"annex b") {
    return Fail("H.264 packet format names changed unexpectedly.");
  }

  return 0;
}

int VerifyH264PacketStoreHandoff() {
  const std::vector<uint8_t> annex_b = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
      0x00, 0x00, 0x01,       0x68, 0xce, 0x06,
      0x00, 0x00, 0x01,       0x65, 0x88, 0x84,
  };

  olouie::encode::MfHardwareH264EncodedPacket packet;
  packet.pts_ns = 33333333;
  packet.duration_ns = 16666667;
  packet.keyframe = true;
  packet.data = annex_b;
  packet.bitstream = olouie::encode::InspectMfHardwareH264Bitstream(packet.data);

  olouie::record::PacketMetadata metadata;
  std::wstring error;
  if (!olouie::encode::BuildH264PacketMetadata(9, packet, &metadata, &error)) {
    std::wcerr << L"H.264 metadata build failed: " << error << L'\n';
    return 1;
  }

  if (metadata.track_id != 9 ||
      metadata.codec_id != olouie::record::CodecId::H264 ||
      metadata.flags != olouie::record::PacketFlagKeyframe ||
      metadata.pts_ns != packet.pts_ns ||
      metadata.dts_ns != packet.pts_ns ||
      metadata.duration_ns != packet.duration_ns) {
    return Fail("H.264 packet metadata mapping is incorrect.");
  }

  olouie::encode::H264PacketStoreConfig config;
  if (!olouie::encode::BuildH264PacketStoreConfig(
          9, packet.bitstream.config, &config, &error)) {
    std::wcerr << L"H.264 PacketStore config build failed: " << error << L'\n';
    return 1;
  }

  if (!config.IsReady() || config.track_id != 9 ||
      config.packet_format != olouie::encode::MfHardwareH264PacketFormat::AnnexB ||
      config.sps != packet.bitstream.config.sps ||
      config.pps != packet.bitstream.config.pps ||
      config.avcc_extradata != packet.bitstream.config.avcc_extradata) {
    return Fail("H.264 PacketStore config handoff is incorrect.");
  }

  olouie::encode::MfHardwareH264EncodedPacket invalid_packet;
  if (olouie::encode::BuildH264PacketMetadata(9, invalid_packet, &metadata,
                                              &error)) {
    return Fail("H.264 metadata build should reject missing payloads.");
  }

  olouie::encode::H264PacketStoreConfig invalid_config;
  if (olouie::encode::BuildH264PacketStoreConfig(
          9, olouie::encode::MfHardwareH264ConfigRecord{}, &invalid_config,
          &error)) {
    return Fail("H.264 config handoff should reject missing AVCC extradata.");
  }

  if (olouie::encode::AppendH264Packet(nullptr, 9, packet, &error)) {
    return Fail("H.264 packet append should reject null PacketStore.");
  }
  error.clear();

  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieH264PacketTests";
  const auto session_dir = root / L"session";
  std::filesystem::remove_all(root);

  const std::array tracks{
      olouie::record::TrackDefinition{9, olouie::record::CodecId::H264},
  };
  auto store = olouie::record::PacketStore::Create(session_dir, tracks, &error);
  if (!store.IsWritable()) {
    std::wcerr << L"PacketStore create for H.264 failed: " << error << L'\n';
    return 1;
  }

  if (!olouie::encode::AppendH264Packet(&store, 9, packet, &error)) {
    std::wcerr << L"H.264 packet append failed: " << error << L'\n';
    return 1;
  }

  auto second_packet = packet;
  second_packet.pts_ns += packet.duration_ns;
  second_packet.keyframe = false;
  second_packet.data = {0x00, 0x00, 0x01, 0x41, 0x9a, 0x22};
  second_packet.bitstream =
      olouie::encode::InspectMfHardwareH264Bitstream(second_packet.data);
  if (!olouie::encode::AppendH264Packet(&store, 9, second_packet, &error)) {
    std::wcerr << L"Second H.264 packet append failed: " << error << L'\n';
    return 1;
  }

  const auto index = store.SnapshotIndex();
  if (index.size() != 2 ||
      index[0].metadata.codec_id != olouie::record::CodecId::H264 ||
      index[0].metadata.track_id != 9 ||
      !index[0].IsKeyframe() ||
      index[0].payload_size != packet.data.size() ||
      index[1].metadata.pts_ns != second_packet.pts_ns ||
      index[1].IsKeyframe()) {
    return Fail("H.264 PacketStore index entry is incorrect.");
  }

  const auto range =
      store.QueryRange(packet.pts_ns, second_packet.pts_ns + second_packet.duration_ns,
                       true);
  if (range.actual_start_ns != packet.pts_ns || range.packets.size() != 2 ||
      range.packets[0].metadata.track_id != 9) {
    return Fail("H.264 PacketStore keyframe range query is incorrect.");
  }

  std::vector<std::byte> payload;
  if (!store.ReadPayload(range.packets[0], &payload, &error) ||
      !BytesEqual(payload, packet.data)) {
    std::wcerr << L"H.264 payload readback failed: " << error << L'\n';
    return 1;
  }
  if (!store.ReadPayload(range.packets[1], &payload, &error) ||
      !BytesEqual(payload, second_packet.data)) {
    std::wcerr << L"Second H.264 payload readback failed: " << error << L'\n';
    return 1;
  }

  if (olouie::encode::AppendH264Packet(&store, 10, packet, &error)) {
    return Fail("H.264 packet append should reject unknown PacketStore tracks.");
  }

  store.Close();
  auto recovered = olouie::record::PacketStore::Recover(session_dir, &error);
  const auto recovered_index = recovered.SnapshotIndex();
  if (recovered_index.size() != 2 ||
      recovered_index[0].metadata.track_id != 9 ||
      recovered_index[0].metadata.codec_id != olouie::record::CodecId::H264 ||
      !recovered_index[0].IsKeyframe() ||
      recovered_index[1].metadata.pts_ns != second_packet.pts_ns) {
    return Fail("Recovered H.264 PacketStore index is incorrect.");
  }

  std::filesystem::remove_all(root);
  return 0;
}

int VerifySyntheticVideoRecordingSessionBoundary() {
  olouie::encode::SyntheticVideoRecordingSessionOptions options;
  options.video_track_id = 0;
  olouie::encode::SyntheticVideoRecordingSession invalid_session(options);
  auto result = invalid_session.Prepare(nullptr, nullptr, nullptr);
  if (result.status != olouie::encode::VideoRecordingSessionStatus::InvalidConfig ||
      result.Succeeded() || invalid_session.IsPrepared()) {
    return Fail("Video session should reject an invalid track id.");
  }

  options.video_track_id = 1;
  olouie::encode::SyntheticVideoRecordingSession session(options);
  result = session.SubmitGeneratedFrame(0, 1);
  if (result.status != olouie::encode::VideoRecordingSessionStatus::InvalidState ||
      result.Succeeded()) {
    return Fail("Video session should require prepare before submit.");
  }

  result = session.Prepare(nullptr, nullptr, nullptr);
  if (result.status != olouie::encode::VideoRecordingSessionStatus::InvalidState ||
      result.Succeeded() || session.IsPrepared()) {
    return Fail("Video session should reject missing runtime dependencies.");
  }

  if (session.stats().submitted_frame_count != 0 ||
      session.stats().drained_packet_count != 0 ||
      session.stats().appended_packet_count != 0 ||
      session.config().IsReady()) {
    return Fail("Video session should remain inactive after failed prepare.");
  }

  if (std::wstring(olouie::encode::VideoRecordingSessionStatusName(
          olouie::encode::VideoRecordingSessionStatus::PacketStoreFailed)) !=
      L"packet store failed") {
    return Fail("Video session status names changed unexpectedly.");
  }

  olouie::encode::BgraVideoRecordingSessionOptions bgra_options;
  olouie::encode::BgraVideoRecordingSession invalid_bgra_session(
      bgra_options);
  result = {};
  auto bgra_result =
      invalid_bgra_session.Prepare(nullptr, nullptr, nullptr, nullptr);
  if (bgra_result.status !=
          olouie::encode::VideoRecordingSessionStatus::InvalidConfig ||
      bgra_result.Succeeded() || invalid_bgra_session.IsPrepared()) {
    return Fail("BGRA video session should reject missing source dimensions.");
  }

  bgra_options.source_width = 1920;
  bgra_options.source_height = 1080;
  olouie::encode::BgraVideoRecordingSession bgra_session(bgra_options);
  bgra_result = bgra_session.SubmitBgraFrame(nullptr, 0, 1);
  if (bgra_result.status !=
          olouie::encode::VideoRecordingSessionStatus::InvalidState ||
      bgra_result.Succeeded()) {
    return Fail("BGRA video session should require prepare before submit.");
  }

  bgra_result = bgra_session.Prepare(nullptr, nullptr, nullptr, nullptr);
  if (bgra_result.status !=
          olouie::encode::VideoRecordingSessionStatus::InvalidState ||
      bgra_result.Succeeded() || bgra_session.IsPrepared()) {
    return Fail("BGRA video session should reject missing runtime dependencies.");
  }

  if (bgra_session.stats().converted_frame_count != 0 ||
      bgra_session.stats().submitted_frame_count != 0 ||
      bgra_session.stats().drained_packet_count != 0 ||
      bgra_session.stats().appended_packet_count != 0 ||
      bgra_session.config().IsReady() ||
      bgra_session.conversion_plan().IsValid() ||
      bgra_session.nv12_texture() != nullptr) {
    return Fail("BGRA video session should remain inactive after failed prepare.");
  }

  if (std::wstring(olouie::encode::VideoRecordingSessionStatusName(
          olouie::encode::VideoRecordingSessionStatus::ConvertFailed)) !=
      L"convert failed") {
    return Fail("BGRA video session status names changed unexpectedly.");
  }

  return 0;
}

int VerifyVideoRecordingSetupBoundary() {
  olouie::encode::VideoRecordingPreflightOptions options;
  options.video_track_id = 7;
  options.source_width = 1920;
  options.source_height = 1080;
  options.queue_capacity = 3;
  options.drain_frame_budget = 2;
  options.session_drain_timeout_ms = 3000;
  options.live = olouie::encode::VideoLiveCaptureEncodeOptions{
      std::chrono::milliseconds(100), std::chrono::milliseconds(10), 3, 2,
      10000000, true};

  if (olouie::encode::BuildVideoRecordingPreflight(options, nullptr).status !=
      olouie::encode::VideoRecordingSetupStatus::InvalidConfig) {
    return Fail("Video setup preflight should reject missing output.");
  }

  olouie::encode::VideoRecordingPreflight preflight;
  auto invalid_options = options;
  invalid_options.video_track_id = 0;
  auto result = olouie::encode::BuildVideoRecordingPreflight(
      invalid_options, &preflight);
  if (result.status != olouie::encode::VideoRecordingSetupStatus::InvalidConfig ||
      result.Succeeded() || preflight.IsUsable()) {
    return Fail("Video setup preflight should reject an invalid track id.");
  }

  invalid_options = options;
  invalid_options.encoder_config.width = 1919;
  result = olouie::encode::BuildVideoRecordingPreflight(
      invalid_options, &preflight);
  if (result.status !=
          olouie::encode::VideoRecordingSetupStatus::EncoderConfigInvalid ||
      result.encoder_config_result.status !=
          olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig ||
      result.Succeeded() || preflight.IsUsable()) {
    return Fail("Video setup preflight should surface invalid encoder configs.");
  }

  invalid_options = options;
  invalid_options.live.max_frames_per_drain_tick = 0;
  result = olouie::encode::BuildVideoRecordingPreflight(
      invalid_options, &preflight);
  if (result.status != olouie::encode::VideoRecordingSetupStatus::InvalidConfig ||
      result.Succeeded() || preflight.IsUsable()) {
    return Fail("Video setup preflight should reject invalid live options.");
  }

  result = olouie::encode::BuildVideoRecordingPreflight(options, &preflight);
  if (!result.Succeeded() || !preflight.IsUsable() ||
      !preflight.chain_config.IsValid() ||
      preflight.video_track.track_id != 7 ||
      preflight.video_track.codec_id != olouie::record::CodecId::H264 ||
      preflight.encoder_config.width != 1920 ||
      preflight.chain_config.queue_options.capacity != 3 ||
      preflight.chain_config.queue_options.overflow_policy !=
          olouie::capture::VideoFrameOverflowPolicy::KeepNewest ||
      preflight.chain_config.session_options.video_track_id != 7 ||
      preflight.chain_config.session_options.drain_timeout_ms != 3000 ||
      preflight.chain_config.session_options.source_width != 1920 ||
      preflight.chain_config.session_options.source_height != 1080 ||
      preflight.chain_config.drain_frame_budget != 2 ||
      !preflight.chain_config.worker_options.timebase.IsValid() ||
      preflight.chain_config.worker_options.timebase.qpc_frequency() !=
          10000000 ||
      preflight.chain_config.worker_options.fallback_frame_duration_ns !=
          16666666 ||
      preflight.live_options.duration != std::chrono::milliseconds(100) ||
      preflight.live_options.max_frames_per_drain_tick != 2) {
    return Fail("Video setup preflight should assemble chain/live options.");
  }

  auto explicit_origin_options = options;
  explicit_origin_options.live.start_timebase_on_first_frame = false;
  explicit_origin_options.live.use_explicit_timebase_origin = true;
  explicit_origin_options.live.timebase_origin_ticks = 987654321;
  result = olouie::encode::BuildVideoRecordingPreflight(
      explicit_origin_options, &preflight);
  if (!result.Succeeded() ||
      preflight.chain_config.worker_options.timebase.session_start_qpc() !=
          987654321) {
    return Fail("Video preflight should preserve an explicit session origin.");
  }

  explicit_origin_options.live.start_timebase_on_first_frame = true;
  result = olouie::encode::BuildVideoRecordingPreflight(
      explicit_origin_options, &preflight);
  if (result.status != olouie::encode::VideoRecordingSetupStatus::InvalidConfig) {
    return Fail("Video preflight should reject competing timebase origins.");
  }

  result = olouie::encode::BuildVideoRecordingPreflight(options, &preflight);
  if (!result.Succeeded()) {
    return Fail("Video setup preflight should recover after invalid options.");
  }

  olouie::encode::VideoRecordingSessionSetup setup;
  auto setup_result = olouie::encode::BuildVideoRecordingSessionSetup(
      preflight, nullptr, nullptr, nullptr, nullptr, &setup);
  if (setup_result.status !=
          olouie::encode::VideoRecordingSetupStatus::InvalidConfig ||
      setup_result.Succeeded() || setup.IsConfigured()) {
    return Fail("Video setup should reject missing runtime dependencies.");
  }

  if (olouie::encode::BuildVideoRecordingSessionSetup(
          preflight, nullptr, nullptr, nullptr, nullptr, nullptr)
          .status != olouie::encode::VideoRecordingSetupStatus::InvalidConfig) {
    return Fail("Video setup should reject missing setup output.");
  }

  preflight.Reset();
  if (preflight.IsUsable()) {
    return Fail("Video setup preflight reset should clear usability.");
  }

  if (std::wstring(olouie::encode::VideoRecordingSetupStatusName(
          olouie::encode::VideoRecordingSetupStatus::SessionCreateFailed)) !=
      L"session create failed") {
    return Fail("Video setup status names changed unexpectedly.");
  }

  return 0;
}

int VerifyVideoRecordingRunSessionBoundary() {
  olouie::encode::VideoRecordingRunSessionOptions options;
  options.monitor = reinterpret_cast<HMONITOR>(1);
  options.preflight.video_track_id = 7;
  options.preflight.source_width = 1920;
  options.preflight.source_height = 1080;
  options.preflight.queue_capacity = 3;
  options.preflight.drain_frame_budget = 2;
  options.preflight.session_drain_timeout_ms = 3000;
  options.preflight.live = olouie::encode::VideoLiveCaptureEncodeOptions{
      std::chrono::milliseconds(100), std::chrono::milliseconds(10), 3, 2,
      10000000, true};

  olouie::encode::VideoRecordingRunSession session(options);
  auto result = session.Run();
  if (result.status !=
          olouie::encode::VideoRecordingRunSessionStatus::InvalidState ||
      result.Succeeded()) {
    return Fail("Video recording run session should require preflight first.");
  }

  result = session.Preflight();
  if (!result.Succeeded() || !session.IsPreflighted() ||
      session.IsPrepared() || session.encode_chain() != nullptr ||
      session.preflight().video_track.track_id != 7 ||
      session.preflight().chain_config.queue_options.capacity != 3 ||
      session.last_result().status !=
          olouie::encode::VideoRecordingRunSessionStatus::Success) {
    return Fail("Video recording run session preflight state is incorrect.");
  }

  result = session.Run();
  if (result.status !=
          olouie::encode::VideoRecordingRunSessionStatus::InvalidState ||
      result.Succeeded()) {
    return Fail("Video recording run session should require prepare before run.");
  }

  result = session.Prepare(nullptr, nullptr, nullptr, nullptr);
  if (result.status !=
          olouie::encode::VideoRecordingRunSessionStatus::InvalidConfig ||
      result.setup_result.status !=
          olouie::encode::VideoRecordingSetupStatus::InvalidConfig ||
      result.Succeeded() || session.IsPrepared() ||
      session.encoder_session() != nullptr ||
      session.packet_store() != nullptr ||
      session.encode_chain() != nullptr) {
    return Fail("Video recording run session should surface setup failures.");
  }

  session.Reset();
  if (session.IsPreflighted() || session.IsPrepared() ||
      session.encode_chain() != nullptr ||
      session.packet_store() != nullptr) {
    return Fail("Video recording run session reset should clear state.");
  }

  auto invalid_options = options;
  invalid_options.preflight.encoder_config.width = 1919;
  olouie::encode::VideoRecordingRunSession invalid_session(invalid_options);
  result = invalid_session.Preflight();
  if (result.status !=
          olouie::encode::VideoRecordingRunSessionStatus::PreflightFailed ||
      result.setup_result.status !=
          olouie::encode::VideoRecordingSetupStatus::EncoderConfigInvalid ||
      result.Succeeded() || invalid_session.IsPreflighted()) {
    return Fail("Video recording run session should surface preflight failures.");
  }

  if (std::wstring(olouie::encode::VideoRecordingRunSessionStatusName(
          olouie::encode::VideoRecordingRunSessionStatus::RunFailed)) !=
      L"run failed") {
    return Fail("Video recording run session status names changed unexpectedly.");
  }

  return 0;
}

int VerifyVideoRecordingBootstrapBoundary() {
  olouie::encode::VideoRecordingBootstrapOptions options;
  options.packet_store_session_dir =
      std::filesystem::temp_directory_path() / L"O'LouieVideoBootstrapTests" /
      L"session";
  options.preflight.video_track_id = 7;
  options.preflight.encoder_config.width = 1920;
  options.preflight.encoder_config.height = 1080;
  options.preflight.queue_capacity = 3;
  options.preflight.drain_frame_budget = 2;
  options.preflight.session_drain_timeout_ms = 3000;
  options.preflight.live = olouie::encode::VideoLiveCaptureEncodeOptions{
      std::chrono::milliseconds(100), std::chrono::milliseconds(10), 3, 2,
      10000000, true};

  if (olouie::encode::BuildVideoRecordingBootstrapSession(options, nullptr)
          .status !=
      olouie::encode::VideoRecordingBootstrapStatus::InvalidConfig) {
    return Fail("Video bootstrap should reject missing output.");
  }

  olouie::encode::VideoRecordingBootstrapSession bootstrap;
  auto invalid_options = options;
  invalid_options.packet_store_session_dir.clear();
  auto result = olouie::encode::BuildVideoRecordingBootstrapSession(
      invalid_options, &bootstrap);
  if (result.status !=
          olouie::encode::VideoRecordingBootstrapStatus::InvalidConfig ||
      result.Succeeded() || bootstrap.IsPrepared()) {
    return Fail("Video bootstrap should reject missing PacketStore path.");
  }

  invalid_options = options;
  invalid_options.preflight.encoder_config.width = 1919;
  result = olouie::encode::BuildVideoRecordingBootstrapSession(
      invalid_options, &bootstrap);
  if (result.status !=
          olouie::encode::VideoRecordingBootstrapStatus::
              RecordingPreflightFailed ||
      result.encoder_config_result.status !=
          olouie::encode::MfHardwareH264EncoderProbeStatus::InvalidConfig ||
      result.Succeeded() || bootstrap.IsPrepared()) {
    return Fail("Video bootstrap should validate encoder config before runtime.");
  }

  invalid_options = options;
  invalid_options.monitor = reinterpret_cast<HMONITOR>(static_cast<intptr_t>(-1));
  result = olouie::encode::BuildVideoRecordingBootstrapSession(
      invalid_options, &bootstrap);
  if (result.status !=
          olouie::encode::VideoRecordingBootstrapStatus::MonitorUnavailable ||
      result.Succeeded() || bootstrap.IsPrepared()) {
    return Fail("Video bootstrap should reject unavailable monitors.");
  }

  bootstrap.Reset();
  if (bootstrap.IsPrepared() || bootstrap.monitor.handle != nullptr ||
      bootstrap.d3d.IsValid() || bootstrap.encoder_session != nullptr ||
      bootstrap.packet_store.IsWritable() ||
      bootstrap.recording_session != nullptr) {
    return Fail("Video bootstrap reset should clear owned runtime state.");
  }

  if (std::wstring(olouie::encode::VideoRecordingBootstrapStatusName(
          olouie::encode::VideoRecordingBootstrapStatus::EncoderInitFailed)) !=
      L"encoder init failed") {
    return Fail("Video bootstrap status names changed unexpectedly.");
  }

  std::filesystem::remove_all(
      std::filesystem::temp_directory_path() / L"O'LouieVideoBootstrapTests");
  return 0;
}

int VerifyVideoRecordingMetadataBoundary() {
  olouie::encode::VideoRecordingMetadataInputs inputs;
  inputs.packet_store_session_dir =
      std::filesystem::temp_directory_path() / L"O'LouieVideoMetadataTests" /
      L"session";
  inputs.packet_file_path = inputs.packet_store_session_dir / L"packets.dat";
  inputs.video_track =
      olouie::record::TrackDefinition{7, olouie::record::CodecId::H264};
  inputs.requested_config.width = 1920;
  inputs.requested_config.height = 1080;
  inputs.h264.track_id = 7;
  inputs.h264.packet_format =
      olouie::encode::MfHardwareH264PacketFormat::AnnexB;
  inputs.h264.sps = {0x67, 0x42, 0x00, 0x1f};
  inputs.h264.pps = {0x68, 0xce, 0x06};
  inputs.h264.avcc_extradata = {
      0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67,
      0x42, 0x00, 0x1f, 0x01, 0x00, 0x03, 0x68, 0xce, 0x06,
  };
  inputs.encoder_info.encoder = olouie::encode::MfHardwareH264EncoderInfo{
      L"Fake Hardware H.264 Encoder", L"{fake-clsid}", 7};
  inputs.encoder_info.media_type.width = 1920;
  inputs.encoder_info.media_type.height = 1080;
  inputs.encoder_info.media_type.fps_numerator = 60;
  inputs.encoder_info.media_type.fps_denominator = 1;
  inputs.encoder_info.media_type.bitrate_bps = 20000000;
  inputs.encoder_info.media_type.gop_frame_count = 120;
  inputs.encoder_info.media_type.h264_profile = 100;
  inputs.encoder_info.media_type.max_b_frames = 0;
  inputs.encoder_info.d3d11_aware = true;
  inputs.encoder_info.device_manager_attached = true;
  inputs.encoder_info.async_transform = true;
  inputs.encoder_info.async_unlocked = true;
  inputs.encoder_info.codec_api_available = true;
  inputs.monitor.handle = reinterpret_cast<HMONITOR>(1);
  inputs.monitor.monitor_rect = RECT{0, 0, 1920, 1080};
  inputs.monitor.work_rect = inputs.monitor.monitor_rect;
  inputs.monitor.primary = true;
  inputs.monitor.device_name = L"DISPLAY1";
  inputs.conversion_plan.source_width = 1920;
  inputs.conversion_plan.source_height = 1080;
  inputs.conversion_plan.output_width = 1920;
  inputs.conversion_plan.output_height = 1080;

  if (olouie::encode::BuildVideoRecordingMetadata(inputs, nullptr).status !=
      olouie::encode::VideoRecordingMetadataStatus::InvalidConfig) {
    return Fail("Video metadata should reject missing output.");
  }

  olouie::encode::VideoRecordingMetadata metadata;
  auto result =
      olouie::encode::BuildVideoRecordingMetadata(inputs, &metadata);
  if (!result.Succeeded() || !metadata.IsReady() ||
      metadata.packet_store_session_dir != inputs.packet_store_session_dir ||
      metadata.packet_file_path != inputs.packet_file_path ||
      metadata.video_track.track_id != 7 ||
      metadata.h264.avcc_extradata != inputs.h264.avcc_extradata ||
      metadata.encoder_name != L"Fake Hardware H.264 Encoder" ||
      metadata.encoder_clsid != L"{fake-clsid}" ||
      metadata.encoder_enumeration_flags != 7 ||
      !metadata.d3d11_aware || !metadata.device_manager_attached ||
      !metadata.async_transform || !metadata.async_unlocked ||
      !metadata.codec_api_available ||
      metadata.monitor_device_name != L"DISPLAY1" ||
      !metadata.monitor_primary ||
      metadata.monitor_left != 0 || metadata.monitor_right != 1920 ||
      metadata.source_width != 1920 || metadata.source_height != 1080 ||
      metadata.output_width != 1920 || metadata.output_height != 1080 ||
      metadata.media_type.gop_frame_count != 120) {
    return Fail("Video metadata should map runtime fields.");
  }

  const auto manifest =
      olouie::encode::BuildVideoRecordingSessionManifest(metadata);
  if (!manifest.IsReady() ||
      manifest.session_dir != inputs.packet_store_session_dir ||
      manifest.packet_file_path != inputs.packet_file_path ||
      manifest.video.track_id != 7 ||
      manifest.video.codec_id != olouie::record::CodecId::H264 ||
      manifest.video.h264_packet_format != L"annex_b" ||
      manifest.video.h264_avcc_extradata != inputs.h264.avcc_extradata ||
      manifest.video.encoder_name != metadata.encoder_name ||
      manifest.video.media_width != 1920 ||
      manifest.video.source_width != 1920 ||
      manifest.video.output_width != 1920) {
    return Fail("Video metadata should build a ready session manifest.");
  }

  if (olouie::encode::BuildVideoRecordingSessionManifest(
          olouie::encode::VideoRecordingMetadata{})
          .IsReady()) {
    return Fail("Video metadata should not build a ready manifest from empty data.");
  }

  auto invalid_inputs = inputs;
  invalid_inputs.h264.avcc_extradata.clear();
  result = olouie::encode::BuildVideoRecordingMetadata(
      invalid_inputs, &metadata);
  if (result.status !=
          olouie::encode::VideoRecordingMetadataStatus::MissingH264Config ||
      result.Succeeded() || metadata.IsReady()) {
    return Fail("Video metadata should require ready H.264 config.");
  }

  invalid_inputs = inputs;
  invalid_inputs.conversion_plan.output_width = 0;
  result = olouie::encode::BuildVideoRecordingMetadata(
      invalid_inputs, &metadata);
  if (result.status !=
          olouie::encode::VideoRecordingMetadataStatus::MissingRuntimeInfo ||
      result.Succeeded() || metadata.IsReady()) {
    return Fail("Video metadata should require runtime dimensions.");
  }

  olouie::encode::VideoRecordingBootstrapSession empty_bootstrap;
  result = olouie::encode::BuildVideoRecordingMetadata(
      empty_bootstrap, &metadata);
  if (result.status !=
          olouie::encode::VideoRecordingMetadataStatus::MissingRuntimeInfo ||
      result.Succeeded() || metadata.IsReady()) {
    return Fail("Video metadata should reject an unprepared bootstrap.");
  }

  if (std::wstring(olouie::encode::VideoRecordingMetadataStatusName(
          olouie::encode::VideoRecordingMetadataStatus::MissingH264Config)) !=
      L"missing H.264 config") {
    return Fail("Video metadata status names changed unexpectedly.");
  }

  std::filesystem::remove_all(
      std::filesystem::temp_directory_path() / L"O'LouieVideoMetadataTests");
  return 0;
}

int VerifyVideoRecordingRuntimeFaultClassification() {
  using olouie::encode::BgraVideoRecordingSessionResult;
  using olouie::encode::MfHardwareH264EncoderDrainStatus;
  using olouie::encode::MfHardwareH264EncoderFrameSubmitStatus;
  using olouie::encode::VideoRecordingRuntimeFaultKind;

  BgraVideoRecordingSessionResult result;
  if (result.RuntimeFaultKind() != VideoRecordingRuntimeFaultKind::None ||
      result.RequiresPreservationFinalization()) {
    return Fail("An ordinary unconfigured result should not be a runtime fault.");
  }

  result.convert_result.device_fault =
      olouie::graphics::ClassifyD3D11DeviceFault(
          E_FAIL, DXGI_ERROR_DEVICE_RESET, L"video processor blit");
  if (result.RuntimeFaultKind() !=
          VideoRecordingRuntimeFaultKind::D3D11DeviceLost ||
      !result.RequiresPreservationFinalization()) {
    return Fail("A conversion device reset should require preservation finalization.");
  }

  result = {};
  result.submit_result.status =
      MfHardwareH264EncoderFrameSubmitStatus::ProcessInputFailed;
  if (result.RuntimeFaultKind() !=
          VideoRecordingRuntimeFaultKind::HardwareEncoderFailed) {
    return Fail("A hardware encoder input failure should be a runtime fault.");
  }

  result = {};
  result.drain_result.status = MfHardwareH264EncoderDrainStatus::TimedOut;
  if (result.RuntimeFaultKind() !=
          VideoRecordingRuntimeFaultKind::HardwareEncoderFailed ||
      !olouie::encode::IsMfHardwareH264EncoderRuntimeFailure(
          MfHardwareH264EncoderDrainStatus::ProcessOutputFailed) ||
      olouie::encode::IsMfHardwareH264EncoderRuntimeFailure(
          MfHardwareH264EncoderFrameSubmitStatus::InvalidArgument) ||
      std::wstring(olouie::encode::VideoRecordingRuntimeFaultKindName(
          VideoRecordingRuntimeFaultKind::D3D11DeviceLost)) !=
          L"D3D11 device lost") {
    return Fail("Hardware encoder drain fault classification is incorrect.");
  }
  return 0;
}

int VerifyEventDrivenVideoEncodeThread() {
  struct FakeBackendState {
    std::mutex mutex;
    std::condition_variable drained;
    uint32_t queued = 0;
    uint64_t processed = 0;
  };
  auto state = std::make_shared<FakeBackendState>();

  olouie::encode::VideoEncodeThreadBackend backend;
  backend.is_prepared = [] { return true; };
  backend.queue_frame = [state](olouie::capture::OwnedVideoFrame) {
    std::lock_guard lock(state->mutex);
    ++state->queued;
    return olouie::capture::VideoFrameQueuePushResult{
        olouie::capture::VideoFrameQueuePushStatus::Queued};
  };
  backend.queued_frame_count = [state] {
    std::lock_guard lock(state->mutex);
    return state->queued;
  };
  backend.drain_queued_frames =
      [state](size_t maximum_frames) {
        olouie::encode::VideoEncodeWorkerResult result;
        result.status = olouie::encode::VideoEncodeWorkerStatus::Success;
        {
          std::lock_guard lock(state->mutex);
          const auto drained = static_cast<uint32_t>(
              std::min<size_t>(state->queued, maximum_frames));
          state->queued -= drained;
          state->processed += drained;
          result.popped_frame_count = drained;
          result.processed_frame_count = drained;
          result.remaining_frame_count = state->queued;
        }
        state->drained.notify_all();
        return result;
      };

  olouie::encode::VideoEncodeThreadOptions worker_options;
  worker_options.max_frames_per_batch = 2;
  worker_options.performance_mode =
      olouie::performance::CapturePerformanceMode::CaptureFirst;
  olouie::encode::VideoEncodeThread worker(std::move(backend),
                                            worker_options);
  if (!worker.Start().Accepted() ||
      worker.Start().status !=
          olouie::encode::VideoEncodeThreadCommandStatus::AlreadyRunning) {
    return Fail("Event-driven video worker lifecycle start handling failed.");
  }

  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level{};
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, device.put(), &feature_level, context.put());
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), &feature_level, context.put());
  }
  if (FAILED(hr)) {
    return Fail("Event-driven video worker test could not create a D3D11 device.");
  }
  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = 2;
  texture_desc.Height = 2;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  winrt::com_ptr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, nullptr,
                                     texture.put()))) {
    return Fail("Event-driven video worker test could not create a frame texture.");
  }

  olouie::capture::OwnedVideoFrame frame;
  frame.texture = texture;
  frame.width = 2;
  frame.height = 2;
  frame.timestamp_ticks = 1;
  if (!worker.OnCapturedVideoFrame(std::move(frame)).Accepted()) {
    return Fail("Event-driven video worker should accept a valid captured frame.");
  }
  {
    std::unique_lock lock(state->mutex);
    if (!state->drained.wait_for(lock, std::chrono::seconds(1),
                                 [state] { return state->processed == 1; })) {
      return Fail("Event-driven video worker did not wake and drain promptly.");
    }
  }
  auto snapshot = worker.Snapshot();
  const auto telemetry_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (snapshot.stats.drained_frame_count != 1 &&
         std::chrono::steady_clock::now() < telemetry_deadline) {
    std::this_thread::yield();
    snapshot = worker.Snapshot();
  }
  if (snapshot.state != olouie::encode::VideoEncodeThreadState::Running ||
      snapshot.stats.received_frame_count != 1 ||
      snapshot.stats.accepted_frame_count != 1 ||
      snapshot.stats.drained_frame_count != 1 ||
      snapshot.stats.notification_count != 1 ||
      snapshot.stats.wake_count == 0 || !snapshot.scheduling.attempted ||
      snapshot.scheduling.plan.mode !=
          olouie::performance::CapturePerformanceMode::CaptureFirst ||
      snapshot.scheduling.plan.workload !=
          olouie::performance::MultimediaThreadWorkload::VideoEncode) {
    return Fail("Event-driven video worker telemetry is incomplete.");
  }
  if (!worker.StopAndDrain().Accepted() ||
      worker.StopAndDrain().status !=
          olouie::encode::VideoEncodeThreadCommandStatus::NotRunning) {
    return Fail("Event-driven video worker stop/drain handling failed.");
  }
  return 0;
}

}  // namespace

int main() {
  if (const int result = VerifyCapturePerformancePolicy(); result != 0) {
    return result;
  }

  if (const int result = VerifyMfHardwareH264EncoderConfigValidation();
      result != 0) {
    return result;
  }

  if (const int result = VerifyMfHardwareH264EncoderProbeBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyMfHardwareH264EncoderSessionPlan();
      result != 0) {
    return result;
  }

  if (const int result = VerifyMfHardwareH264BitstreamInspection();
      result != 0) {
    return result;
  }

  if (const int result = VerifyH264PacketStoreHandoff(); result != 0) {
    return result;
  }

  if (const int result = VerifySyntheticVideoRecordingSessionBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyVideoRecordingSetupBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyVideoRecordingRunSessionBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyVideoRecordingBootstrapBoundary();
      result != 0) {
    return result;
  }

  if (const int result = VerifyVideoRecordingMetadataBoundary(); result != 0) {
    return result;
  }

  if (const int result = VerifyVideoRecordingRuntimeFaultClassification();
      result != 0) {
    return result;
  }

  if (const int result = VerifyEventDrivenVideoEncodeThread(); result != 0) {
    return result;
  }

  return 0;
}
