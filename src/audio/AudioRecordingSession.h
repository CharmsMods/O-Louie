#pragma once

#include <chrono>
#include <string>

#include "audio/AudioCaptureEncodeSetup.h"
#include "audio/AudioLiveCaptureEncode.h"
#include "record/PacketStore.h"

namespace olouie::audio {

enum class AudioRecordingSessionStatus {
  Success,
  InvalidConfig,
  InvalidState,
  PreflightFailed,
  SetupFailed,
  RunFailed,
};

struct AudioRecordingSessionOptions {
  AudioCaptureEncodePreflightOptions preflight;
  AudioCaptureEncodeSessionSetupOptions setup;
  AudioLiveCaptureEncodeOptions live;

  AudioCaptureEncodeFormatProvider format_provider = nullptr;
  AudioCaptureEncodeAacEncoderFactory encoder_factory = nullptr;
  AudioLiveCaptureSourceFactory live_source_factory = nullptr;
};

struct AudioRecordingSessionResult {
  AudioRecordingSessionStatus status =
      AudioRecordingSessionStatus::InvalidConfig;
  std::wstring message;
  AudioCaptureEncodeSetupResult setup_result;
  AudioLiveCaptureEncodeRunResult live_result;

  bool Succeeded() const noexcept;
};

class AudioRecordingSession final {
 public:
  explicit AudioRecordingSession(AudioRecordingSessionOptions options);

  AudioRecordingSessionResult Preflight();
  AudioRecordingSessionResult Prepare(record::PacketStore* packet_store);
  AudioRecordingSessionResult Run();
  AudioRecordingSessionResult RunForDuration(
      std::chrono::milliseconds duration);
  void Reset();

  bool IsPreflighted() const noexcept;
  bool IsPrepared() const noexcept;

  const AudioRecordingSessionOptions& options() const noexcept;
  const AudioCaptureEncodePreflight& preflight() const noexcept;
  const AudioCaptureEncodeSessionSetup& setup() const noexcept;
  AudioEncodeSession* encode_session() const noexcept;
  record::PacketStore* packet_store() const noexcept;
  const AudioLiveCaptureEncodeResult& last_live_result() const noexcept;
  const AudioRecordingSessionResult& last_result() const noexcept;

 private:
  AudioRecordingSessionOptions options_;
  record::PacketStore* packet_store_ = nullptr;
  AudioCaptureEncodePreflight preflight_;
  AudioCaptureEncodeSessionSetup setup_;
  AudioLiveCaptureEncodeResult last_live_result_;
  AudioRecordingSessionResult last_result_;
};

const wchar_t* AudioRecordingSessionStatusName(
    AudioRecordingSessionStatus status) noexcept;

}  // namespace olouie::audio
