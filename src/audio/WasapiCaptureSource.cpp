#include "audio/WasapiCaptureSource.h"

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace olouie::audio {
namespace {

constexpr DWORD kStartupTimeoutMs = 5000;
constexpr DWORD kLiveEventWaitMs = 100;
constexpr DWORD kPollingWaitMs = 10;
constexpr REFERENCE_TIME kBufferDuration = 10000000;
constexpr DWORD kInitialRecoveryDelayMs = 100;
constexpr DWORD kMaxRecoveryDelayMs = 2000;
constexpr uint32_t kEventDefaultEndpointPollIterations = 10;
constexpr uint32_t kPollingDefaultEndpointPollIterations = 100;

struct ComApartment {
  HRESULT result = E_FAIL;

  ComApartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

  ~ComApartment() {
    if (SUCCEEDED(result)) {
      CoUninitialize();
    }
  }

  bool Initialized() const noexcept {
    return SUCCEEDED(result);
  }
};

struct WaveFormatHolder {
  WAVEFORMATEX* value = nullptr;

  ~WaveFormatHolder() {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
  }
};

struct EventHandle {
  HANDLE value = nullptr;

  ~EventHandle() {
    if (value != nullptr) {
      CloseHandle(value);
    }
  }
};

WasapiCaptureSourceResult Result(WasapiCaptureSourceStatus status,
                                 std::wstring message) {
  WasapiCaptureSourceResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

std::wstring HResultText(HRESULT result) {
  wchar_t buffer[24]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

std::wstring SystemErrorText(DWORD error) {
  wchar_t buffer[24]{};
  swprintf_s(buffer, L"%lu", static_cast<unsigned long>(error));
  return buffer;
}

bool IsSupportedWasapiSource(CapturedAudioSource source) noexcept {
  return IsCapturedAudioSourceValid(source) &&
         (source.kind == AudioTrackKind::SystemLoopback ||
          source.kind == AudioTrackKind::Microphone);
}

const wchar_t* SourceName(CapturedAudioSource source) noexcept {
  switch (source.kind) {
    case AudioTrackKind::SystemLoopback:
      return L"system loopback";
    case AudioTrackKind::Microphone:
      return L"microphone";
    case AudioTrackKind::ProcessLoopback:
      return L"process loopback";
    case AudioTrackKind::DefaultMixed:
      return L"default mixed audio";
  }

  return L"unknown audio source";
}

EDataFlow EndpointFlowForSource(CapturedAudioSource source) noexcept {
  return source.kind == AudioTrackKind::Microphone ? eCapture : eRender;
}

DWORD StreamFlagsForSource(CapturedAudioSource source,
                           bool event_callback) noexcept {
  DWORD flags = source.kind == AudioTrackKind::SystemLoopback
                    ? AUDCLNT_STREAMFLAGS_LOOPBACK
                    : 0;
  if (event_callback) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  return flags;
}

PcmSampleEncoding DetectEncoding(const WAVEFORMATEX& format) {
  if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return PcmSampleEncoding::Float;
  }

  if (format.wFormatTag == WAVE_FORMAT_PCM) {
    return format.wBitsPerSample == 8 ? PcmSampleEncoding::UnsignedInteger
                                      : PcmSampleEncoding::SignedInteger;
  }

  if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
      format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const auto& extensible =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
      return PcmSampleEncoding::Float;
    }
    if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
      return format.wBitsPerSample == 8 ? PcmSampleEncoding::UnsignedInteger
                                        : PcmSampleEncoding::SignedInteger;
    }
  }

  return PcmSampleEncoding::Unknown;
}

PcmStreamFormat StreamFormatFromWaveFormat(const WAVEFORMATEX& format) {
  return MakePcmStreamFormat(format.nSamplesPerSec, format.nChannels,
                             format.wBitsPerSample, format.nBlockAlign,
                             format.nAvgBytesPerSec, DetectEncoding(format));
}

WasapiCaptureSourceResult CreateDefaultEndpoint(
    CapturedAudioSource source,
    winrt::com_ptr<IMMDevice>* endpoint,
    std::wstring* endpoint_id) {
  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  HRESULT hr =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       IID_PPV_ARGS(enumerator.put()));
  if (FAILED(hr)) {
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  L"Could not create MMDeviceEnumerator: " +
                      HResultText(hr) + L".");
  }

  hr = enumerator->GetDefaultAudioEndpoint(EndpointFlowForSource(source),
                                           eConsole, endpoint->put());
  if (FAILED(hr)) {
    return Result(WasapiCaptureSourceStatus::EndpointUnavailable,
                  std::wstring(L"No default ") + SourceName(source) +
                      L" endpoint is available: " + HResultText(hr) + L".");
  }

  if (endpoint_id != nullptr) {
    LPWSTR id = nullptr;
    hr = (*endpoint)->GetId(&id);
    if (FAILED(hr) || id == nullptr) {
      if (id != nullptr) {
        CoTaskMemFree(id);
      }
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"Could not read the default audio endpoint id: " +
                        HResultText(hr) + L".");
    }
    *endpoint_id = id;
    CoTaskMemFree(id);
  }

  return Result(WasapiCaptureSourceStatus::Success, L"");
}

WasapiCaptureSourceResult CheckDefaultEndpoint(
    CapturedAudioSource source,
    const std::wstring& active_endpoint_id,
    bool* changed,
    HRESULT* failure_hresult) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (failure_hresult != nullptr) {
    *failure_hresult = S_OK;
  }

  winrt::com_ptr<IMMDevice> endpoint;
  std::wstring current_id;
  auto result = CreateDefaultEndpoint(source, &endpoint, &current_id);
  if (!result.Succeeded()) {
    if (failure_hresult != nullptr) {
      *failure_hresult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    return result;
  }
  if (current_id != active_endpoint_id) {
    if (changed != nullptr) {
      *changed = true;
    }
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  std::wstring(L"Windows default ") + SourceName(source) +
                      L" endpoint changed; capture will reconnect.");
  }
  return Result(WasapiCaptureSourceStatus::Success, L"");
}

WasapiCaptureSourceResult DrainWasapiPackets(
    IAudioCaptureClient* capture_client,
    CapturedAudioSource source,
    PcmCaptureStats* stats,
    ICapturedPcmSink* sink,
    HRESULT* failure_hresult) {
  if (failure_hresult != nullptr) {
    *failure_hresult = S_OK;
  }
  for (;;) {
    UINT32 packet_frames = 0;
    HRESULT hr = capture_client->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
      if (failure_hresult != nullptr) {
        *failure_hresult = hr;
      }
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"IAudioCaptureClient::GetNextPacketSize failed: " +
                        HResultText(hr) + L".");
    }

    if (packet_frames == 0) {
      return Result(WasapiCaptureSourceStatus::Success, L"");
    }

    BYTE* data = nullptr;
    DWORD flags = 0;
    UINT64 device_position = 0;
    UINT64 qpc_position = 0;
    hr = capture_client->GetBuffer(&data, &packet_frames, &flags,
                                   &device_position, &qpc_position);
    if (FAILED(hr)) {
      if (failure_hresult != nullptr) {
        *failure_hresult = hr;
      }
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"IAudioCaptureClient::GetBuffer failed: " +
                        HResultText(hr) + L".");
    }

    PcmPacketTiming timing;
    std::wstring timing_error;
    if (!BuildPcmPacketTiming(device_position, qpc_position, packet_frames,
                              stats->format.sample_rate, &timing,
                              &timing_error)) {
      capture_client->ReleaseBuffer(packet_frames);
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"Could not build WASAPI packet timing: " +
                        timing_error);
    }

    const PcmPacketInfo packet{
        timing,
        (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
        (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
        (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0};
    stats->AddPacket(packet);

    CapturedPcmSinkResult delivered;
    delivered.status = CapturedPcmSinkStatus::Success;
    if (sink != nullptr) {
      const auto byte_count =
          static_cast<size_t>(packet_frames) * stats->format.block_align;
      std::span<const std::byte> pcm_bytes;
      if (!packet.silent && data != nullptr) {
        pcm_bytes =
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(data),
                                       byte_count);
      }

      delivered = DispatchCapturedPcm(
          sink, CapturedPcmPacket{source, stats->format, packet, pcm_bytes});
    }

    hr = capture_client->ReleaseBuffer(packet_frames);
    if (FAILED(hr)) {
      if (failure_hresult != nullptr) {
        *failure_hresult = hr;
      }
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"IAudioCaptureClient::ReleaseBuffer failed: " +
                        HResultText(hr) + L".");
    }

    if (!delivered.Succeeded()) {
      return Result(WasapiCaptureSourceStatus::SinkError,
                    std::wstring(L"Captured ") + SourceName(source) +
                        L" PCM sink failed: " + delivered.message);
    }
  }
}

WasapiCaptureSourceResult InitializeAudioClient(
    CapturedAudioSource source,
    IAudioClient* audio_client,
    WAVEFORMATEX* mix_format,
    EventHandle* capture_event,
    bool* used_event_callback) {
  *used_event_callback = false;
  DWORD stream_flags = StreamFlagsForSource(source, true);
  HRESULT hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                        kBufferDuration, 0, mix_format,
                                        nullptr);

  if (SUCCEEDED(hr)) {
    capture_event->value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (capture_event->value == nullptr) {
      return Result(WasapiCaptureSourceStatus::ThreadError,
                    std::wstring(L"Could not create WASAPI ") +
                        SourceName(source) + L" event.");
    }

    hr = audio_client->SetEventHandle(capture_event->value);
    if (FAILED(hr)) {
      return Result(WasapiCaptureSourceStatus::DeviceError,
                    L"IAudioClient::SetEventHandle failed: " +
                        HResultText(hr) + L".");
    }

    *used_event_callback = true;
    return Result(WasapiCaptureSourceStatus::Success, L"");
  }

  stream_flags = StreamFlagsForSource(source, false);
  hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                kBufferDuration, 0, mix_format, nullptr);
  if (FAILED(hr)) {
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  std::wstring(L"IAudioClient::Initialize failed for ") +
                      SourceName(source) + L": " + HResultText(hr) + L".");
  }

  return Result(WasapiCaptureSourceStatus::Success, L"");
}

}  // namespace

bool detail::IsWasapiCaptureFailureRecoverable(int32_t result) noexcept {
  return result == AUDCLNT_E_DEVICE_INVALIDATED ||
         result == AUDCLNT_E_SERVICE_NOT_RUNNING ||
         result == AUDCLNT_E_RESOURCES_INVALIDATED ||
         result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
         result == RPC_E_DISCONNECTED;
}

uint32_t detail::WasapiCaptureRecoveryDelayMs(
    uint32_t retry_number) noexcept {
  uint32_t delay = kInitialRecoveryDelayMs;
  for (uint32_t attempt = 0;
       attempt < retry_number && delay < kMaxRecoveryDelayMs; ++attempt) {
    delay = std::min<uint32_t>(delay * 2, kMaxRecoveryDelayMs);
  }
  return delay;
}

bool WasapiCaptureSourceResult::Succeeded() const noexcept {
  return status == WasapiCaptureSourceStatus::Success;
}

WasapiCaptureSource::WasapiCaptureSource(
    CapturedAudioSource source,
    performance::CapturePerformanceMode performance_mode)
    : source_(source), performance_mode_(performance_mode) {}

WasapiCaptureSource::~WasapiCaptureSource() {
  Stop();
}

WasapiCaptureSourceResult WasapiCaptureSource::Start(ICapturedPcmSink* sink) {
  if (sink == nullptr) {
    return Result(WasapiCaptureSourceStatus::InvalidConfig,
                  L"WASAPI capture source needs a captured PCM sink.");
  }

  if (!IsSupportedWasapiSource(source_)) {
    return Result(WasapiCaptureSourceStatus::InvalidConfig,
                  std::wstring(L"WASAPI live capture does not support ") +
                      SourceName(source_) + L" source index " +
                      std::to_wstring(source_.source_index) + L".");
  }
  if (!performance::IsValidCapturePerformanceMode(performance_mode_)) {
    return Result(WasapiCaptureSourceStatus::InvalidConfig,
                  L"WASAPI capture performance mode is invalid.");
  }

  {
    std::lock_guard lock(mutex_);
    if (worker_.joinable() || running_.load()) {
      return Result(WasapiCaptureSourceStatus::AlreadyRunning,
                    L"WASAPI capture source is already running.");
    }

    stats_ = {};
    last_result_ = Result(WasapiCaptureSourceStatus::Success, L"");
    sink_ = sink;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    startup_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr || startup_event_ == nullptr) {
      CloseEventsLocked();
      sink_ = nullptr;
      return Result(WasapiCaptureSourceStatus::ThreadError,
                    L"Could not create WASAPI capture lifecycle events.");
    }
  }

  try {
    std::thread worker(&WasapiCaptureSource::CaptureThreadMain, this);
    std::lock_guard lock(mutex_);
    worker_ = std::move(worker);
  } catch (const std::system_error& error) {
    std::lock_guard lock(mutex_);
    CloseEventsLocked();
    sink_ = nullptr;
    return Result(WasapiCaptureSourceStatus::ThreadError,
                  L"Could not start WASAPI capture thread: " +
                      std::to_wstring(error.code().value()) + L".");
  }

  HANDLE startup_event = nullptr;
  {
    std::lock_guard lock(mutex_);
    startup_event = static_cast<HANDLE>(startup_event_);
  }

  const DWORD wait = WaitForSingleObject(startup_event, kStartupTimeoutMs);
  if (wait != WAIT_OBJECT_0) {
    Stop();
    return Result(WasapiCaptureSourceStatus::ThreadError,
                  L"WASAPI capture source did not finish startup in time.");
  }

  const auto started = LastResult();
  if (!started.Succeeded()) {
    Stop();
  }
  return started;
}

void WasapiCaptureSource::Stop() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (stop_event_ != nullptr) {
      SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (worker_.joinable()) {
      worker = std::move(worker_);
    } else {
      running_.store(false);
      sink_ = nullptr;
      CloseEventsLocked();
      return;
    }
  }

  if (worker.get_id() != std::this_thread::get_id()) {
    worker.join();
  }

  {
    std::lock_guard lock(mutex_);
    running_.store(false);
    sink_ = nullptr;
    CloseEventsLocked();
  }
}

bool WasapiCaptureSource::IsRunning() const noexcept {
  return running_.load();
}

CapturedAudioSource WasapiCaptureSource::source() const noexcept {
  return source_;
}

PcmCaptureStats WasapiCaptureSource::SnapshotStats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

WasapiCaptureSourceResult WasapiCaptureSource::LastResult() const {
  std::lock_guard lock(mutex_);
  return last_result_;
}

WasapiCaptureSourceResult WasapiCaptureSource::RunCaptureAttempt(
    ICapturedPcmSink* sink,
    void* stop_event_value,
    PcmCaptureStats* stats,
    bool recovery_attempt,
    bool* started,
    bool* recoverable,
    bool* default_device_changed,
    bool* startup_signaled) {
  *started = false;
  *recoverable = false;
  *default_device_changed = false;
  HANDLE stop_event = static_cast<HANDLE>(stop_event_value);

  winrt::com_ptr<IMMDevice> endpoint;
  std::wstring endpoint_id;
  auto result = CreateDefaultEndpoint(source_, &endpoint, &endpoint_id);
  if (!result.Succeeded()) {
    *recoverable =
        result.status == WasapiCaptureSourceStatus::EndpointUnavailable;
    return result;
  }

  winrt::com_ptr<IAudioClient> audio_client;
  HRESULT hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  audio_client.put_void());
  if (FAILED(hr)) {
    *recoverable = detail::IsWasapiCaptureFailureRecoverable(hr);
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  L"Could not activate IAudioClient: " + HResultText(hr) +
                      L".");
  }

  WaveFormatHolder mix_format;
  hr = audio_client->GetMixFormat(&mix_format.value);
  if (FAILED(hr) || mix_format.value == nullptr) {
    *recoverable = detail::IsWasapiCaptureFailureRecoverable(hr);
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  L"IAudioClient::GetMixFormat failed: " + HResultText(hr) +
                      L".");
  }

  const auto capture_format = StreamFormatFromWaveFormat(*mix_format.value);
  if (!capture_format.IsValid()) {
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  std::wstring(L"Default ") + SourceName(source_) +
                      L" endpoint returned an unsupported PCM mix format.");
  }
  if (stats->format.IsValid() &&
      !SamePcmStreamFormat(stats->format, capture_format)) {
    ++stats->capture_format_change_count;
  }
  stats->format = capture_format;

  EventHandle capture_event;
  bool used_event_callback = false;
  result = InitializeAudioClient(source_, audio_client.get(), mix_format.value,
                                 &capture_event, &used_event_callback);
  if (!result.Succeeded()) {
    *recoverable =
        result.status == WasapiCaptureSourceStatus::DeviceError;
    return result;
  }
  stats->used_event_callback = used_event_callback;

  winrt::com_ptr<IAudioCaptureClient> capture_client;
  hr = audio_client->GetService(__uuidof(IAudioCaptureClient),
                                capture_client.put_void());
  if (FAILED(hr)) {
    *recoverable = detail::IsWasapiCaptureFailureRecoverable(hr);
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  L"Could not get IAudioCaptureClient: " + HResultText(hr) +
                      L".");
  }

  hr = audio_client->Start();
  if (FAILED(hr)) {
    *recoverable = detail::IsWasapiCaptureFailureRecoverable(hr);
    return Result(WasapiCaptureSourceStatus::DeviceError,
                  L"IAudioClient::Start failed: " + HResultText(hr) + L".");
  }

  *started = true;
  if (recovery_attempt) {
    ++stats->restart_success_count;
  }
  PublishStats(*stats);
  if (startup_signaled != nullptr && !*startup_signaled) {
    SignalStartup(Result(WasapiCaptureSourceStatus::Success, L""),
                  startup_signaled);
  }

  bool ok = true;
  HRESULT failure_hresult = S_OK;
  uint32_t endpoint_poll_iteration = 0;
  const uint32_t endpoint_poll_limit =
      used_event_callback ? kEventDefaultEndpointPollIterations
                          : kPollingDefaultEndpointPollIterations;

  if (used_event_callback) {
    HANDLE handles[2] = {stop_event, capture_event.value};
    while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
      const DWORD wait =
          WaitForMultipleObjects(2, handles, FALSE, kLiveEventWaitMs);
      if (wait == WAIT_OBJECT_0) {
        break;
      }
      if (wait == WAIT_FAILED) {
        result = Result(WasapiCaptureSourceStatus::ThreadError,
                        L"Waiting for WASAPI capture event failed: " +
                            SystemErrorText(GetLastError()) + L".");
        ok = false;
        break;
      }

      result = DrainWasapiPackets(capture_client.get(), source_, stats, sink,
                                  &failure_hresult);
      PublishStats(*stats);
      if (!result.Succeeded()) {
        *recoverable = detail::IsWasapiCaptureFailureRecoverable(
            failure_hresult);
        ok = false;
        break;
      }

      if (++endpoint_poll_iteration >= endpoint_poll_limit) {
        endpoint_poll_iteration = 0;
        result = CheckDefaultEndpoint(source_, endpoint_id,
                                      default_device_changed,
                                      &failure_hresult);
        if (!result.Succeeded()) {
          *recoverable = true;
          ok = false;
          break;
        }
      }
    }
  } else {
    while (WaitForSingleObject(stop_event, kPollingWaitMs) == WAIT_TIMEOUT) {
      result = DrainWasapiPackets(capture_client.get(), source_, stats, sink,
                                  &failure_hresult);
      PublishStats(*stats);
      if (!result.Succeeded()) {
        *recoverable = detail::IsWasapiCaptureFailureRecoverable(
            failure_hresult);
        ok = false;
        break;
      }

      if (++endpoint_poll_iteration >= endpoint_poll_limit) {
        endpoint_poll_iteration = 0;
        result = CheckDefaultEndpoint(source_, endpoint_id,
                                      default_device_changed,
                                      &failure_hresult);
        if (!result.Succeeded()) {
          *recoverable = true;
          ok = false;
          break;
        }
      }
    }
  }

  if (ok) {
    result = DrainWasapiPackets(capture_client.get(), source_, stats, sink,
                                &failure_hresult);
    PublishStats(*stats);
    if (!result.Succeeded()) {
      *recoverable = detail::IsWasapiCaptureFailureRecoverable(
          failure_hresult);
    }
  }

  (void)audio_client->Stop();
  return result;
}

void WasapiCaptureSource::CaptureThreadMain() {
  bool startup_signaled = false;
  ICapturedPcmSink* sink = nullptr;
  HANDLE stop_event = nullptr;
  {
    std::lock_guard lock(mutex_);
    sink = sink_;
    stop_event = static_cast<HANDLE>(stop_event_);
  }

  ComApartment com;
  if (!com.Initialized()) {
    SignalStartup(Result(WasapiCaptureSourceStatus::DeviceError,
                         L"COM initialization failed on WASAPI capture "
                         L"thread: " + HResultText(com.result) + L"."),
                  &startup_signaled);
    return;
  }

  PcmCaptureStats local_stats;
  performance::MultimediaThreadRegistration scheduling;
  local_stats.scheduling = scheduling.Register(
      performance::BuildMultimediaThreadSchedulingPlan(
          performance_mode_,
          performance::MultimediaThreadWorkload::AudioCapture));
  PublishStats(local_stats);
  WasapiCaptureSourceResult result =
      Result(WasapiCaptureSourceStatus::Success, L"");
  bool recovery_attempt = false;
  uint32_t recovery_retry_number = 0;

  for (;;) {
    if (recovery_attempt) {
      ++local_stats.restart_attempt_count;
      PublishStats(local_stats);
    }

    bool attempt_started = false;
    bool recoverable = false;
    bool default_device_changed = false;
    result = RunCaptureAttempt(
        sink, stop_event, &local_stats, recovery_attempt, &attempt_started,
        &recoverable, &default_device_changed, &startup_signaled);

    if (!startup_signaled) {
      SignalStartup(result, &startup_signaled);
      break;
    }
    if (result.Succeeded() ||
        WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
      result = Result(WasapiCaptureSourceStatus::Success, L"");
      break;
    }
    if (!recoverable) {
      break;
    }

    if (attempt_started) {
      if (recovery_attempt) {
        recovery_retry_number = 0;
      }
      if (default_device_changed) {
        ++local_stats.default_device_change_count;
      } else {
        ++local_stats.endpoint_invalidation_count;
      }
      PublishStats(local_stats);
    }

    const DWORD wait = WaitForSingleObject(
        stop_event,
        detail::WasapiCaptureRecoveryDelayMs(recovery_retry_number));
    if (wait == WAIT_OBJECT_0) {
      result = Result(WasapiCaptureSourceStatus::Success, L"");
      break;
    }
    if (wait == WAIT_FAILED) {
      result = Result(WasapiCaptureSourceStatus::ThreadError,
                      L"Waiting to reconnect the audio endpoint failed: " +
                          SystemErrorText(GetLastError()) + L".");
      break;
    }
    recovery_attempt = true;
    ++recovery_retry_number;
  }

  PublishStats(local_stats);
  SetLastResult(result);
  running_.store(false);
}

void WasapiCaptureSource::PublishStats(const PcmCaptureStats& stats) {
  std::lock_guard lock(mutex_);
  stats_ = stats;
}

void WasapiCaptureSource::SetLastResult(WasapiCaptureSourceResult result) {
  std::lock_guard lock(mutex_);
  last_result_ = std::move(result);
}

void WasapiCaptureSource::SignalStartup(WasapiCaptureSourceResult result,
                                        bool* startup_signaled) {
  const bool success = result.Succeeded();
  HANDLE startup_event = nullptr;
  {
    std::lock_guard lock(mutex_);
    last_result_ = std::move(result);
    running_.store(success);
    startup_event = static_cast<HANDLE>(startup_event_);
  }

  if (startup_event != nullptr) {
    SetEvent(startup_event);
  }
  if (startup_signaled != nullptr) {
    *startup_signaled = true;
  }
}

void WasapiCaptureSource::CloseEventsLocked() noexcept {
  if (stop_event_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(stop_event_));
    stop_event_ = nullptr;
  }
  if (startup_event_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(startup_event_));
    startup_event_ = nullptr;
  }
}

const wchar_t* WasapiCaptureSourceStatusName(
    WasapiCaptureSourceStatus status) noexcept {
  switch (status) {
    case WasapiCaptureSourceStatus::Success:
      return L"success";
    case WasapiCaptureSourceStatus::InvalidConfig:
      return L"invalid config";
    case WasapiCaptureSourceStatus::AlreadyRunning:
      return L"already running";
    case WasapiCaptureSourceStatus::EndpointUnavailable:
      return L"endpoint unavailable";
    case WasapiCaptureSourceStatus::DeviceError:
      return L"device error";
    case WasapiCaptureSourceStatus::ThreadError:
      return L"thread error";
    case WasapiCaptureSourceStatus::SinkError:
      return L"sink error";
  }

  return L"unknown";
}

}  // namespace olouie::audio
