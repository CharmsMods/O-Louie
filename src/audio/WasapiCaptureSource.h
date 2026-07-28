#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "audio/AudioSource.h"
#include "audio/CapturedPcmSink.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

namespace detail {

bool IsWasapiCaptureFailureRecoverable(int32_t hresult) noexcept;
uint32_t WasapiCaptureRecoveryDelayMs(uint32_t retry_number) noexcept;

}  // namespace detail

enum class WasapiCaptureSourceStatus {
  Success,
  InvalidConfig,
  AlreadyRunning,
  EndpointUnavailable,
  DeviceError,
  ThreadError,
  SinkError,
};

struct WasapiCaptureSourceResult {
  WasapiCaptureSourceStatus status =
      WasapiCaptureSourceStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

class WasapiCaptureSource final {
 public:
  explicit WasapiCaptureSource(
      CapturedAudioSource source,
      performance::CapturePerformanceMode performance_mode =
          performance::CapturePerformanceMode::Balanced);
  ~WasapiCaptureSource();

  WasapiCaptureSource(const WasapiCaptureSource&) = delete;
  WasapiCaptureSource& operator=(const WasapiCaptureSource&) = delete;

  WasapiCaptureSourceResult Start(ICapturedPcmSink* sink);
  void Stop();

  bool IsRunning() const noexcept;
  CapturedAudioSource source() const noexcept;
  PcmCaptureStats SnapshotStats() const;
  WasapiCaptureSourceResult LastResult() const;

 private:
  void CaptureThreadMain();
  WasapiCaptureSourceResult RunCaptureAttempt(
      ICapturedPcmSink* sink,
      void* stop_event,
      PcmCaptureStats* stats,
      bool recovery_attempt,
      bool* started,
      bool* recoverable,
      bool* default_device_changed,
      bool* startup_signaled);
  void PublishStats(const PcmCaptureStats& stats);
  void SetLastResult(WasapiCaptureSourceResult result);
  void SignalStartup(WasapiCaptureSourceResult result,
                     bool* startup_signaled);
  void CloseEventsLocked() noexcept;

  CapturedAudioSource source_;
  performance::CapturePerformanceMode performance_mode_ =
      performance::CapturePerformanceMode::Balanced;
  std::atomic_bool running_ = false;
  mutable std::mutex mutex_;
  std::thread worker_;
  void* stop_event_ = nullptr;
  void* startup_event_ = nullptr;
  ICapturedPcmSink* sink_ = nullptr;
  PcmCaptureStats stats_;
  WasapiCaptureSourceResult last_result_;
};

const wchar_t* WasapiCaptureSourceStatusName(
    WasapiCaptureSourceStatus status) noexcept;

}  // namespace olouie::audio
