#include "audio/MicMonitorSession.h"

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkeydef.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace olouie::audio {
namespace {

constexpr REFERENCE_TIME kCaptureBufferDuration = 10000000;
constexpr REFERENCE_TIME kRenderBufferDuration = 1000000;
constexpr DWORD kWorkerPollMilliseconds = 40;
constexpr float kMinimumMeterDbfs = -60.0f;
constexpr float kMeterDecayDbPerSecond = 18.0f;
constexpr float kClipThresholdDbfs = -0.1f;
constexpr auto kClipHoldDuration = std::chrono::milliseconds(750);

struct ComApartment {
  HRESULT result = E_FAIL;

  ComApartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(result)) {
      CoUninitialize();
    }
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

struct CoTaskMemString {
  LPWSTR value = nullptr;

  ~CoTaskMemString() {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
  }
};

struct PropVariantHolder {
  PROPVARIANT value{};

  PropVariantHolder() { PropVariantInit(&value); }
  ~PropVariantHolder() { PropVariantClear(&value); }
};

std::wstring HResultText(HRESULT result) {
  wchar_t buffer[24]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

bool IsDeviceInvalidated(HRESULT result) noexcept {
  return result == AUDCLNT_E_DEVICE_INVALIDATED ||
         result == AUDCLNT_E_RESOURCES_INVALIDATED;
}

PcmSampleEncoding DetectEncoding(const WAVEFORMATEX& format) noexcept {
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

PcmStreamFormat StreamFormatFromWaveFormat(
    const WAVEFORMATEX& format) noexcept {
  return MakePcmStreamFormat(format.nSamplesPerSec, format.nChannels,
                             format.wBitsPerSample, format.nBlockAlign,
                             format.nAvgBytesPerSec, DetectEncoding(format));
}

std::wstring ReadEndpointId(IMMDevice* device) {
  CoTaskMemString id;
  if (device == nullptr || FAILED(device->GetId(&id.value)) ||
      id.value == nullptr) {
    return {};
  }
  return id.value;
}

std::wstring ReadEndpointName(IMMDevice* device) {
  winrt::com_ptr<IPropertyStore> properties;
  if (device == nullptr ||
      FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
    return {};
  }
  PropVariantHolder name;
  if (FAILED(properties->GetValue(PKEY_Device_FriendlyName, &name.value)) ||
      name.value.vt != VT_LPWSTR || name.value.pwszVal == nullptr) {
    return {};
  }
  return name.value.pwszVal;
}

struct ResolvedRenderEndpoint {
  winrt::com_ptr<IMMDevice> device;
  std::wstring id;
  std::wstring name;
  bool fallback = false;
};

HRESULT ResolveDefaultEndpoint(IMMDeviceEnumerator* enumerator,
                               EDataFlow flow,
                               winrt::com_ptr<IMMDevice>* endpoint) {
  if (enumerator == nullptr || endpoint == nullptr) {
    return E_INVALIDARG;
  }
  return enumerator->GetDefaultAudioEndpoint(flow, eConsole,
                                              endpoint->put());
}

HRESULT ResolveRenderEndpoint(IMMDeviceEnumerator* enumerator,
                              const std::wstring& requested_id,
                              bool force_default,
                              ResolvedRenderEndpoint* resolved) {
  if (enumerator == nullptr || resolved == nullptr) {
    return E_INVALIDARG;
  }

  *resolved = {};
  if (!requested_id.empty() && !force_default) {
    winrt::com_ptr<IMMDevice> requested;
    DWORD state = 0;
    const HRESULT get = enumerator->GetDevice(requested_id.c_str(),
                                               requested.put());
    if (SUCCEEDED(get) && requested != nullptr &&
        SUCCEEDED(requested->GetState(&state)) &&
        (state & DEVICE_STATE_ACTIVE) != 0) {
      resolved->device = std::move(requested);
    } else {
      resolved->fallback = true;
    }
  }

  if (resolved->device == nullptr) {
    const HRESULT fallback = ResolveDefaultEndpoint(
        enumerator, eRender, &resolved->device);
    if (FAILED(fallback)) {
      return fallback;
    }
    resolved->fallback = resolved->fallback ||
                         (!requested_id.empty() && force_default);
  }

  resolved->id = ReadEndpointId(resolved->device.get());
  resolved->name = ReadEndpointName(resolved->device.get());
  if (resolved->name.empty()) {
    resolved->name = resolved->id.empty() ? L"Windows default output"
                                          : resolved->id;
  }
  return S_OK;
}

enum class WasapiRunStatus {
  Stopped,
  OutputInvalidated,
  Failed,
};

struct WasapiRunResult {
  WasapiRunStatus status = WasapiRunStatus::Failed;
  std::wstring message;
};

class WasapiMicMonitorBackend final : public IMicMonitorBackend {
 public:
  MicMonitorBackendResult Run(const MicMonitorOptions& options,
                              const std::atomic_bool& stop_requested,
                              UpdateSink update_sink) override {
    ComApartment com;
    if (FAILED(com.result)) {
      return {MicMonitorBackendStatus::Failed,
              L"COM initialization failed on the microphone monitor thread: " +
                  HResultText(com.result) + L"."};
    }

    auto first = RunOnce(options, false, stop_requested, update_sink);
    if (first.status == WasapiRunStatus::OutputInvalidated &&
        !stop_requested.load()) {
      MicMonitorBackendUpdate switching = last_update_;
      switching.message =
          L"The selected output disconnected; switching to Windows Default.";
      update_sink(switching);
      first = RunOnce(options, true, stop_requested, update_sink);
    }

    if (first.status == WasapiRunStatus::Stopped || stop_requested.load()) {
      return {MicMonitorBackendStatus::Stopped, L"Microphone check stopped."};
    }
    return {MicMonitorBackendStatus::Failed, std::move(first.message)};
  }

 private:
  WasapiRunResult RunOnce(const MicMonitorOptions& options,
                          bool force_default,
                          const std::atomic_bool& stop_requested,
                          const UpdateSink& update_sink) {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not create the Windows audio endpoint enumerator: " +
                  HResultText(hr) + L"."};
    }

    winrt::com_ptr<IMMDevice> capture_endpoint;
    hr = ResolveDefaultEndpoint(enumerator.get(), eCapture,
                                &capture_endpoint);
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"No Windows default microphone is available: " +
                  HResultText(hr) + L"."};
    }

    ResolvedRenderEndpoint render_endpoint;
    hr = ResolveRenderEndpoint(enumerator.get(), options.output_device_id,
                               force_default, &render_endpoint);
    if (FAILED(hr) || render_endpoint.device == nullptr) {
      return {WasapiRunStatus::Failed,
              L"No active audio output device is available for microphone "
              L"check playback: " +
                  HResultText(hr) + L"."};
    }

    winrt::com_ptr<IAudioClient> capture_audio;
    winrt::com_ptr<IAudioClient> render_audio;
    hr = capture_endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr, capture_audio.put_void());
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not activate the default microphone: " +
                  HResultText(hr) + L"."};
    }
    hr = render_endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, render_audio.put_void());
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not activate the microphone-check output: " +
                  HResultText(hr) + L"."};
    }

    WaveFormatHolder capture_format;
    hr = capture_audio->GetMixFormat(&capture_format.value);
    if (FAILED(hr) || capture_format.value == nullptr) {
      return {WasapiRunStatus::Failed,
              L"Could not read the default microphone format: " +
                  HResultText(hr) + L"."};
    }
    const PcmStreamFormat pcm_format =
        StreamFormatFromWaveFormat(*capture_format.value);
    if (!pcm_format.IsValid() || pcm_format.block_align == 0) {
      return {WasapiRunStatus::Failed,
              L"The default microphone uses an unsupported PCM format."};
    }

    EventHandle capture_event;
    EventHandle render_event;
    capture_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    render_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (capture_event.value == nullptr || render_event.value == nullptr) {
      return {WasapiRunStatus::Failed,
              L"Could not create microphone-check audio events."};
    }

    hr = capture_audio->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kCaptureBufferDuration, 0, capture_format.value, nullptr);
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not initialize shared microphone capture: " +
                  HResultText(hr) + L"."};
    }
    hr = capture_audio->SetEventHandle(capture_event.value);
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not attach the microphone capture event: " +
                  HResultText(hr) + L"."};
    }

    constexpr DWORD render_flags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = render_audio->Initialize(AUDCLNT_SHAREMODE_SHARED, render_flags,
                                  kRenderBufferDuration, 0,
                                  capture_format.value, nullptr);
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not initialize shared microphone-check playback: " +
                  HResultText(hr) + L"."};
    }
    hr = render_audio->SetEventHandle(render_event.value);
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not attach the microphone-check playback event: " +
                  HResultText(hr) + L"."};
    }

    winrt::com_ptr<IAudioCaptureClient> capture_client;
    winrt::com_ptr<IAudioRenderClient> render_client;
    hr = capture_audio->GetService(__uuidof(IAudioCaptureClient),
                                   capture_client.put_void());
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not open the microphone capture stream: " +
                  HResultText(hr) + L"."};
    }
    hr = render_audio->GetService(__uuidof(IAudioRenderClient),
                                  render_client.put_void());
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not open the microphone-check playback stream: " +
                  HResultText(hr) + L"."};
    }

    UINT32 render_buffer_frames = 0;
    hr = render_audio->GetBufferSize(&render_buffer_frames);
    if (FAILED(hr) || render_buffer_frames == 0) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not read the microphone-check playback buffer size: " +
                  HResultText(hr) + L"."};
    }

    const uint32_t quarter_second_frames =
        std::max<uint32_t>(1, pcm_format.sample_rate / 4);
    const uint32_t queue_capacity =
        std::max<uint32_t>(quarter_second_frames, render_buffer_frames * 2);
    detail::MicMonitorPcmFifo fifo(queue_capacity, pcm_format.block_align);

    BYTE* initial_buffer = nullptr;
    hr = render_client->GetBuffer(render_buffer_frames, &initial_buffer);
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not prime microphone-check playback: " +
                  HResultText(hr) + L"."};
    }
    hr = render_client->ReleaseBuffer(render_buffer_frames,
                                      AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not release the primed playback buffer: " +
                  HResultText(hr) + L"."};
    }

    hr = capture_audio->Start();
    if (FAILED(hr)) {
      return {WasapiRunStatus::Failed,
              L"Could not start microphone capture: " + HResultText(hr) +
                  L"."};
    }
    bool capture_started = true;
    hr = render_audio->Start();
    if (FAILED(hr)) {
      capture_audio->Stop();
      return {IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Could not start microphone-check playback: " +
                  HResultText(hr) + L"."};
    }
    bool render_started = true;

    uint64_t underrun_count = 0;
    uint64_t overflow_count = 0;
    float smoothed_peak = kMinimumMeterDbfs;
    auto last_meter_time = std::chrono::steady_clock::now();
    auto clipping_until = std::chrono::steady_clock::time_point::min();

    auto publish_update = [&](std::wstring message = {}) {
      MicMonitorBackendUpdate update;
      update.monitoring_started = true;
      update.active_output_device_id = render_endpoint.id;
      update.active_output_device_name = render_endpoint.name;
      update.using_fallback_output = render_endpoint.fallback;
      update.peak_dbfs = smoothed_peak;
      update.clipping =
          std::chrono::steady_clock::now() < clipping_until;
      update.underrun_count = underrun_count;
      update.overflow_count = overflow_count;
      update.queued_frame_count = fifo.size_frames();
      update.queue_capacity_frames = fifo.capacity_frames();
      if (!message.empty()) {
        update.message = std::move(message);
      } else if (render_endpoint.fallback) {
        update.message = L"The saved output is unavailable; using " +
                         render_endpoint.name + L".";
      } else {
        update.message = L"Monitoring the Windows default microphone.";
      }
      last_update_ = update;
      update_sink(update);
    };

    publish_update();

    auto update_meter = [&](float measured_peak) {
      const auto now = std::chrono::steady_clock::now();
      const float elapsed =
          std::chrono::duration<float>(now - last_meter_time).count();
      smoothed_peak = std::max(
          measured_peak,
          std::max(kMinimumMeterDbfs,
                   smoothed_peak - (elapsed * kMeterDecayDbPerSecond)));
      last_meter_time = now;
      if (measured_peak >= kClipThresholdDbfs) {
        clipping_until = now + kClipHoldDuration;
      }
    };

    WasapiRunResult run_result{WasapiRunStatus::Stopped,
                               L"Microphone check stopped."};
    HANDLE events[2] = {capture_event.value, render_event.value};
    while (!stop_requested.load()) {
      const DWORD wait = WaitForMultipleObjects(2, events, FALSE,
                                                kWorkerPollMilliseconds);
      if (wait == WAIT_FAILED) {
        run_result = {WasapiRunStatus::Failed,
                      L"Waiting for microphone-check audio events failed."};
        break;
      }

      bool publish = false;
      if (wait == WAIT_OBJECT_0 ||
          WaitForSingleObject(capture_event.value, 0) == WAIT_OBJECT_0) {
        for (;;) {
          UINT32 packet_frames = 0;
          hr = capture_client->GetNextPacketSize(&packet_frames);
          if (FAILED(hr)) {
            run_result = {
                WasapiRunStatus::Failed,
                L"Reading microphone packet availability failed: " +
                    HResultText(hr) + L"."};
            break;
          }
          if (packet_frames == 0) {
            break;
          }

          BYTE* data = nullptr;
          DWORD flags = 0;
          hr = capture_client->GetBuffer(&data, &packet_frames, &flags,
                                         nullptr, nullptr);
          if (FAILED(hr)) {
            run_result = {
                WasapiRunStatus::Failed,
                L"Reading microphone PCM failed: " + HResultText(hr) + L"."};
            break;
          }

          const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
          const size_t byte_count =
              static_cast<size_t>(packet_frames) * pcm_format.block_align;
          const auto bytes =
              silent || data == nullptr
                  ? std::span<const std::byte>()
                  : std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data), byte_count);
          update_meter(MeasurePcmPeakDbfs(pcm_format, bytes, silent));
          if (fifo.Push(reinterpret_cast<const std::byte*>(data),
                        packet_frames, silent)) {
            ++overflow_count;
          }
          hr = capture_client->ReleaseBuffer(packet_frames);
          if (FAILED(hr)) {
            run_result = {
                WasapiRunStatus::Failed,
                L"Releasing microphone PCM failed: " + HResultText(hr) +
                    L"."};
            break;
          }
          publish = true;
        }
        if (run_result.status == WasapiRunStatus::Failed) {
          break;
        }
      }

      if (wait == WAIT_OBJECT_0 + 1 ||
          WaitForSingleObject(render_event.value, 0) == WAIT_OBJECT_0) {
        UINT32 padding = 0;
        hr = render_audio->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
          run_result = {
              IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                      : WasapiRunStatus::Failed,
              L"Reading microphone-check playback availability failed: " +
                  HResultText(hr) + L"."};
          break;
        }
        const UINT32 available =
            padding < render_buffer_frames ? render_buffer_frames - padding : 0;
        if (available != 0) {
          BYTE* destination = nullptr;
          hr = render_client->GetBuffer(available, &destination);
          if (FAILED(hr)) {
            run_result = {
                IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                        : WasapiRunStatus::Failed,
                L"Opening microphone-check playback PCM failed: " +
                    HResultText(hr) + L"."};
            break;
          }
          const uint32_t copied = fifo.Pop(
              reinterpret_cast<std::byte*>(destination), available);
          DWORD release_flags = 0;
          if (copied < available) {
            ++underrun_count;
            const size_t remaining_offset =
                static_cast<size_t>(copied) * pcm_format.block_align;
            const size_t remaining_size =
                static_cast<size_t>(available - copied) *
                pcm_format.block_align;
            std::memset(destination + remaining_offset, 0, remaining_size);
            if (copied == 0) {
              release_flags = AUDCLNT_BUFFERFLAGS_SILENT;
            }
          }
          hr = render_client->ReleaseBuffer(available, release_flags);
          if (FAILED(hr)) {
            run_result = {
                IsDeviceInvalidated(hr) ? WasapiRunStatus::OutputInvalidated
                                        : WasapiRunStatus::Failed,
                L"Submitting microphone-check playback PCM failed: " +
                    HResultText(hr) + L"."};
            break;
          }
          publish = true;
        }
      }

      if (wait == WAIT_TIMEOUT) {
        update_meter(kMinimumMeterDbfs);
        publish = true;
      }
      if (publish) {
        publish_update();
      }
    }

    if (render_started) {
      render_audio->Stop();
    }
    if (capture_started) {
      capture_audio->Stop();
    }
    return run_result;
  }

  MicMonitorBackendUpdate last_update_;
};

std::unique_ptr<IMicMonitorBackend> CreateDefaultBackend() {
  return std::make_unique<WasapiMicMonitorBackend>();
}

MicMonitorCommandResult CommandResult(MicMonitorCommandStatus status,
                                      std::wstring message) {
  MicMonitorCommandResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

double ReadSignedSample(const std::byte* source, uint16_t bits_per_sample) {
  if (bits_per_sample == 16) {
    int16_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value < 0 ? static_cast<double>(value) / 32768.0
                     : static_cast<double>(value) / 32767.0;
  }
  if (bits_per_sample == 24) {
    int32_t value = static_cast<int32_t>(std::to_integer<uint8_t>(source[0])) |
                    (static_cast<int32_t>(
                         std::to_integer<uint8_t>(source[1]))
                     << 8) |
                    (static_cast<int32_t>(
                         std::to_integer<uint8_t>(source[2]))
                     << 16);
    if ((value & 0x00800000) != 0) {
      value |= static_cast<int32_t>(0xFF000000);
    }
    return value < 0 ? static_cast<double>(value) / 8388608.0
                     : static_cast<double>(value) / 8388607.0;
  }
  if (bits_per_sample == 32) {
    int32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value < 0 ? static_cast<double>(value) / 2147483648.0
                     : static_cast<double>(value) / 2147483647.0;
  }
  return 0.0;
}

}  // namespace

namespace detail {

MicMonitorPcmFifo::MicMonitorPcmFifo(uint32_t capacity_frames,
                                     uint16_t block_align)
    : capacity_frames_(capacity_frames),
      block_align_(block_align),
      bytes_(static_cast<size_t>(capacity_frames) * block_align) {}

uint32_t MicMonitorPcmFifo::capacity_frames() const noexcept {
  return capacity_frames_;
}

uint32_t MicMonitorPcmFifo::size_frames() const noexcept {
  return size_frames_;
}

bool MicMonitorPcmFifo::Push(const std::byte* source, uint32_t frame_count,
                             bool silent) {
  if (frame_count == 0 || capacity_frames_ == 0 || block_align_ == 0) {
    return false;
  }

  bool overflowed = false;
  if (frame_count >= capacity_frames_) {
    overflowed = frame_count > capacity_frames_ || size_frames_ != 0;
    const uint32_t skipped = frame_count - capacity_frames_;
    source = source == nullptr
                 ? nullptr
                 : source + static_cast<size_t>(skipped) * block_align_;
    frame_count = capacity_frames_;
    read_frame_ = 0;
    size_frames_ = 0;
  } else if (size_frames_ + frame_count > capacity_frames_) {
    const uint32_t drop = size_frames_ + frame_count - capacity_frames_;
    read_frame_ = (read_frame_ + drop) % capacity_frames_;
    size_frames_ -= drop;
    overflowed = true;
  }

  const uint32_t write_frame =
      (read_frame_ + size_frames_) % capacity_frames_;
  const uint32_t first_frames =
      std::min(frame_count, capacity_frames_ - write_frame);
  WriteFrames(write_frame, first_frames, source, silent);
  if (first_frames < frame_count) {
    const std::byte* second_source =
        source == nullptr
            ? nullptr
            : source + static_cast<size_t>(first_frames) * block_align_;
    WriteFrames(0, frame_count - first_frames, second_source, silent);
  }
  size_frames_ += frame_count;
  return overflowed;
}

uint32_t MicMonitorPcmFifo::Pop(std::byte* destination,
                                uint32_t requested_frames) {
  if (destination == nullptr || requested_frames == 0 || size_frames_ == 0) {
    return 0;
  }
  const uint32_t frame_count = std::min(requested_frames, size_frames_);
  const uint32_t first_frames =
      std::min(frame_count, capacity_frames_ - read_frame_);
  CopyFrames(destination, read_frame_, first_frames);
  if (first_frames < frame_count) {
    CopyFrames(destination + static_cast<size_t>(first_frames) * block_align_,
               0, frame_count - first_frames);
  }
  read_frame_ = (read_frame_ + frame_count) % capacity_frames_;
  size_frames_ -= frame_count;
  return frame_count;
}

void MicMonitorPcmFifo::WriteFrames(uint32_t destination_frame,
                                     uint32_t frame_count,
                                     const std::byte* source, bool silent) {
  const size_t offset = static_cast<size_t>(destination_frame) * block_align_;
  const size_t size = static_cast<size_t>(frame_count) * block_align_;
  if (silent || source == nullptr) {
    std::memset(bytes_.data() + offset, 0, size);
  } else {
    std::memcpy(bytes_.data() + offset, source, size);
  }
}

void MicMonitorPcmFifo::CopyFrames(std::byte* destination,
                                    uint32_t source_frame,
                                    uint32_t frame_count) const {
  const size_t offset = static_cast<size_t>(source_frame) * block_align_;
  const size_t size = static_cast<size_t>(frame_count) * block_align_;
  std::memcpy(destination, bytes_.data() + offset, size);
}

}  // namespace detail

bool MicMonitorCommandResult::Accepted() const noexcept {
  return status == MicMonitorCommandStatus::Accepted;
}

MicMonitorSession::MicMonitorSession(
    MicMonitorBackendFactory backend_factory)
    : backend_factory_(std::move(backend_factory)) {
  if (!backend_factory_) {
    backend_factory_ = [] { return CreateDefaultBackend(); };
  }
}

MicMonitorSession::~MicMonitorSession() { Shutdown(); }

void MicMonitorSession::SetStateSink(StateSink sink) {
  std::lock_guard lock(mutex_);
  state_sink_ = std::move(sink);
}

MicMonitorCommandResult MicMonitorSession::Start(MicMonitorOptions options) {
  ReapCompletedWorker();

  MicMonitorSnapshot published;
  StateSink sink;
  bool thread_failed = false;
  std::wstring thread_failure_message;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) {
      return CommandResult(MicMonitorCommandStatus::ShuttingDown,
                           L"Microphone check is shutting down.");
    }
    if (worker_running_ ||
        snapshot_.state == MicMonitorState::Starting ||
        snapshot_.state == MicMonitorState::Monitoring ||
        snapshot_.state == MicMonitorState::Stopping) {
      return CommandResult(MicMonitorCommandStatus::AlreadyRunning,
                           L"Microphone check is already active.");
    }

    const uint64_t generation = snapshot_.generation + 1;
    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.state = MicMonitorState::Starting;
    snapshot_.requested_output_device_id = options.output_device_id;
    snapshot_.peak_dbfs = kMinimumMeterDbfs;
    snapshot_.message = L"Starting microphone check.";
    stop_requested_.store(false);
    worker_running_ = true;
    try {
      worker_ = std::thread(&MicMonitorSession::WorkerMain, this,
                            std::move(options));
    } catch (const std::system_error& error) {
      worker_running_ = false;
      snapshot_.state = MicMonitorState::Failed;
      snapshot_.message = L"Could not start the microphone-check worker: " +
                          std::to_wstring(error.code().value()) + L".";
      thread_failed = true;
      thread_failure_message = snapshot_.message;
    }
    published = snapshot_;
    sink = state_sink_;
  }

  Publish(published, sink);
  if (thread_failed) {
    return CommandResult(MicMonitorCommandStatus::ThreadError,
                         std::move(thread_failure_message));
  }
  return CommandResult(MicMonitorCommandStatus::Accepted,
                       L"Microphone check is starting.");
}

MicMonitorCommandResult MicMonitorSession::Stop() {
  MicMonitorSnapshot published;
  StateSink sink;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.state != MicMonitorState::Starting &&
        snapshot_.state != MicMonitorState::Monitoring &&
        snapshot_.state != MicMonitorState::Stopping) {
      return CommandResult(MicMonitorCommandStatus::NotRunning,
                           L"Microphone check is not running.");
    }
    stop_requested_.store(true);
    if (snapshot_.state != MicMonitorState::Stopping) {
      snapshot_.state = MicMonitorState::Stopping;
      ++snapshot_.generation;
      snapshot_.message = L"Stopping microphone check.";
    }
    published = snapshot_;
    sink = state_sink_;
  }
  Publish(published, sink);
  return CommandResult(MicMonitorCommandStatus::Accepted,
                       L"Microphone check is stopping.");
}

void MicMonitorSession::Shutdown() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ && !worker_.joinable()) {
      return;
    }
    shutting_down_ = true;
    stop_requested_.store(true);
    state_sink_ = {};
    if (worker_.joinable()) {
      worker = std::move(worker_);
    }
  }
  if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
    worker.join();
  }
  std::lock_guard lock(mutex_);
  worker_running_ = false;
  snapshot_.state = MicMonitorState::Idle;
  snapshot_.message = L"Microphone check stopped.";
}

MicMonitorSnapshot MicMonitorSession::Snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void MicMonitorSession::WorkerMain(MicMonitorOptions options) {
  MicMonitorBackendResult result;
  try {
    std::unique_ptr<IMicMonitorBackend> backend = backend_factory_();
    if (backend == nullptr) {
      result = {MicMonitorBackendStatus::Failed,
                L"Microphone-check audio backend is unavailable."};
    } else {
      result = backend->Run(
          options, stop_requested_,
          [this](const MicMonitorBackendUpdate& update) {
            ApplyBackendUpdate(update);
          });
    }
  } catch (const std::exception&) {
    result = {MicMonitorBackendStatus::Failed,
              L"Microphone-check audio processing failed unexpectedly."};
  }

  MicMonitorSnapshot published;
  StateSink sink;
  {
    std::lock_guard lock(mutex_);
    worker_running_ = false;
    ++snapshot_.generation;
    if (result.status == MicMonitorBackendStatus::Stopped ||
        stop_requested_.load() || shutting_down_) {
      snapshot_.state = MicMonitorState::Idle;
      snapshot_.peak_dbfs = kMinimumMeterDbfs;
      snapshot_.clipping = false;
      snapshot_.queued_frame_count = 0;
      snapshot_.message = result.message.empty()
                              ? L"Microphone check stopped."
                              : std::move(result.message);
    } else {
      snapshot_.state = MicMonitorState::Failed;
      snapshot_.peak_dbfs = kMinimumMeterDbfs;
      snapshot_.clipping = false;
      snapshot_.queued_frame_count = 0;
      snapshot_.message = result.message.empty()
                              ? L"Microphone check failed."
                              : std::move(result.message);
    }
    published = snapshot_;
    sink = state_sink_;
  }
  Publish(published, sink);
}

void MicMonitorSession::ApplyBackendUpdate(
    const MicMonitorBackendUpdate& update) {
  MicMonitorSnapshot published;
  StateSink sink;
  bool noteworthy = false;
  bool generation_changed = false;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.state == MicMonitorState::Stopping || shutting_down_) {
      return;
    }
    if (update.monitoring_started &&
        snapshot_.state == MicMonitorState::Starting) {
      snapshot_.state = MicMonitorState::Monitoring;
      ++snapshot_.generation;
      generation_changed = true;
      noteworthy = true;
    }
    if (snapshot_.active_output_device_id !=
            update.active_output_device_id ||
        snapshot_.active_output_device_name !=
            update.active_output_device_name ||
        snapshot_.using_fallback_output !=
            update.using_fallback_output ||
        snapshot_.message != update.message) {
      noteworthy = true;
    }
    snapshot_.active_output_device_id = update.active_output_device_id;
    snapshot_.active_output_device_name = update.active_output_device_name;
    snapshot_.using_fallback_output = update.using_fallback_output;
    snapshot_.peak_dbfs =
        std::clamp(update.peak_dbfs, kMinimumMeterDbfs, 0.0f);
    snapshot_.clipping = update.clipping;
    snapshot_.underrun_count = update.underrun_count;
    snapshot_.overflow_count = update.overflow_count;
    snapshot_.queued_frame_count = update.queued_frame_count;
    snapshot_.queue_capacity_frames = update.queue_capacity_frames;
    snapshot_.message = update.message;
    if (noteworthy) {
      if (!generation_changed) {
        ++snapshot_.generation;
      }
      published = snapshot_;
      sink = state_sink_;
    }
  }
  if (noteworthy) {
    Publish(published, sink);
  }
}

void MicMonitorSession::Publish(MicMonitorSnapshot snapshot,
                                const StateSink& sink) const {
  if (sink) {
    sink(snapshot);
  }
}

void MicMonitorSession::ReapCompletedWorker() {
  std::thread completed;
  {
    std::lock_guard lock(mutex_);
    if (!worker_running_ && worker_.joinable()) {
      completed = std::move(worker_);
    }
  }
  if (completed.joinable() &&
      completed.get_id() != std::this_thread::get_id()) {
    completed.join();
  }
}

float MeasurePcmPeakDbfs(const PcmStreamFormat& format,
                         std::span<const std::byte> pcm_bytes,
                         bool silent) noexcept {
  if (silent || pcm_bytes.empty() || !format.IsValid() ||
      format.bits_per_sample % 8 != 0) {
    return kMinimumMeterDbfs;
  }
  const size_t bytes_per_sample = format.bits_per_sample / 8;
  if (bytes_per_sample == 0 || pcm_bytes.size() % bytes_per_sample != 0) {
    return kMinimumMeterDbfs;
  }

  double peak = 0.0;
  for (size_t offset = 0; offset < pcm_bytes.size();
       offset += bytes_per_sample) {
    double sample = 0.0;
    const std::byte* source = pcm_bytes.data() + offset;
    if (format.encoding == PcmSampleEncoding::Float &&
        format.bits_per_sample == 32) {
      float value = 0.0f;
      std::memcpy(&value, source, sizeof(value));
      sample = std::isfinite(value) ? static_cast<double>(value) : 0.0;
    } else if (format.encoding == PcmSampleEncoding::Float &&
               format.bits_per_sample == 64) {
      double value = 0.0;
      std::memcpy(&value, source, sizeof(value));
      sample = std::isfinite(value) ? value : 0.0;
    } else if (format.encoding == PcmSampleEncoding::UnsignedInteger &&
               format.bits_per_sample == 8) {
      sample = (static_cast<double>(std::to_integer<uint8_t>(*source)) -
                128.0) /
               128.0;
    } else if (format.encoding == PcmSampleEncoding::SignedInteger) {
      sample = ReadSignedSample(source, format.bits_per_sample);
    }
    peak = std::max(peak, std::abs(sample));
  }

  if (peak <= 0.001) {
    return kMinimumMeterDbfs;
  }
  const double dbfs = 20.0 * std::log10(peak);
  return static_cast<float>(std::clamp(dbfs,
                                       static_cast<double>(kMinimumMeterDbfs),
                                       0.0));
}

const wchar_t* MicMonitorStateName(MicMonitorState state) noexcept {
  switch (state) {
    case MicMonitorState::Idle:
      return L"idle";
    case MicMonitorState::Starting:
      return L"starting";
    case MicMonitorState::Monitoring:
      return L"monitoring";
    case MicMonitorState::Stopping:
      return L"stopping";
    case MicMonitorState::Failed:
      return L"failed";
  }
  return L"unknown";
}

const wchar_t* MicMonitorCommandStatusName(
    MicMonitorCommandStatus status) noexcept {
  switch (status) {
    case MicMonitorCommandStatus::Accepted:
      return L"accepted";
    case MicMonitorCommandStatus::AlreadyRunning:
      return L"already running";
    case MicMonitorCommandStatus::NotRunning:
      return L"not running";
    case MicMonitorCommandStatus::ShuttingDown:
      return L"shutting down";
    case MicMonitorCommandStatus::ThreadError:
      return L"thread error";
  }
  return L"unknown";
}

}  // namespace olouie::audio
