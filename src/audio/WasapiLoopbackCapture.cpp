#include "audio/WasapiLoopbackCapture.h"

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>

namespace olouie::audio {
namespace {

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

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

std::wstring HResultText(HRESULT result) {
  wchar_t buffer[24]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

bool CreateDefaultRenderEndpoint(winrt::com_ptr<IMMDevice>* endpoint,
                                 std::wstring* error) {
  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  HRESULT result =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       IID_PPV_ARGS(enumerator.put()));
  if (FAILED(result)) {
    SetError(error, L"Could not create MMDeviceEnumerator: " +
                        HResultText(result) + L".");
    return false;
  }

  result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                               endpoint->put());
  if (FAILED(result)) {
    SetError(error, L"No default render endpoint is available: " +
                        HResultText(result) + L".");
    return false;
  }

  return true;
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

bool DrainLoopbackPackets(IAudioCaptureClient* capture_client,
                          LoopbackSmokeResult* result,
                          ICapturedPcmSink* sink,
                          std::wstring* error) {
  for (;;) {
    UINT32 packet_frames = 0;
    HRESULT hr = capture_client->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
      SetError(error, L"IAudioCaptureClient::GetNextPacketSize failed: " +
                          HResultText(hr) + L".");
      return false;
    }

    if (packet_frames == 0) {
      return true;
    }

    BYTE* data = nullptr;
    DWORD flags = 0;
    UINT64 device_position = 0;
    UINT64 qpc_position = 0;
    hr = capture_client->GetBuffer(&data, &packet_frames, &flags,
                                   &device_position, &qpc_position);
    if (FAILED(hr)) {
      SetError(error, L"IAudioCaptureClient::GetBuffer failed: " +
                          HResultText(hr) + L".");
      return false;
    }

    PcmPacketTiming timing;
    if (!BuildPcmPacketTiming(device_position, qpc_position, packet_frames,
                              result->format.sample_rate, &timing, error)) {
      capture_client->ReleaseBuffer(packet_frames);
      return false;
    }

    const PcmPacketInfo packet{
        timing,
        (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
        (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
        (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0};
    result->AddPacket(packet);

    CapturedPcmSinkResult delivered;
    delivered.status = CapturedPcmSinkStatus::Success;
    if (sink != nullptr) {
      const auto byte_count =
          static_cast<size_t>(packet_frames) * result->format.block_align;
      std::span<const std::byte> pcm_bytes;
      if (!packet.silent && data != nullptr) {
        pcm_bytes =
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(data),
                                       byte_count);
      }

      delivered = DispatchCapturedPcm(
          sink, CapturedPcmPacket{
                    CapturedAudioSource{AudioTrackKind::SystemLoopback, 0},
                    result->format, packet, pcm_bytes});
    }

    hr = capture_client->ReleaseBuffer(packet_frames);
    if (FAILED(hr)) {
      SetError(error, L"IAudioCaptureClient::ReleaseBuffer failed: " +
                          HResultText(hr) + L".");
      return false;
    }

    if (!delivered.Succeeded()) {
      SetError(error, L"Captured loopback PCM sink failed: " +
                          delivered.message);
      return false;
    }
  }
}

}  // namespace

bool TryGetDefaultRenderLoopbackFormat(PcmStreamFormat* format,
                                       std::wstring* error) {
  if (format == nullptr) {
    SetError(error, L"Default render loopback format needs an output destination.");
    return false;
  }

  *format = {};

  winrt::com_ptr<IMMDevice> endpoint;
  if (!CreateDefaultRenderEndpoint(&endpoint, error)) {
    return false;
  }

  winrt::com_ptr<IAudioClient> audio_client;
  HRESULT hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  audio_client.put_void());
  if (FAILED(hr)) {
    SetError(error, L"Could not activate IAudioClient: " + HResultText(hr) +
                        L".");
    return false;
  }

  WaveFormatHolder mix_format;
  hr = audio_client->GetMixFormat(&mix_format.value);
  if (FAILED(hr) || mix_format.value == nullptr) {
    SetError(error, L"IAudioClient::GetMixFormat failed: " + HResultText(hr) +
                        L".");
    return false;
  }

  *format = StreamFormatFromWaveFormat(*mix_format.value);
  if (!format->IsValid()) {
    SetError(error, L"Default render endpoint returned an unsupported PCM mix format.");
    return false;
  }

  return true;
}

bool RunDefaultRenderLoopbackSmoke(std::chrono::milliseconds duration,
                                   LoopbackSmokeResult* result,
                                   std::wstring* error) {
  return RunDefaultRenderLoopbackSmoke(duration, result, nullptr, error);
}

bool RunDefaultRenderLoopbackSmoke(std::chrono::milliseconds duration,
                                   LoopbackSmokeResult* result,
                                   ICapturedPcmSink* sink,
                                   std::wstring* error) {
  if (result == nullptr) {
    SetError(error, L"Loopback smoke needs an output destination.");
    return false;
  }

  if (duration <= std::chrono::milliseconds(0) ||
      duration > std::chrono::seconds(30)) {
    SetError(error, L"Loopback smoke duration must be between 1 ms and 30 s.");
    return false;
  }

  *result = {};

  winrt::com_ptr<IMMDevice> endpoint;
  if (!CreateDefaultRenderEndpoint(&endpoint, error)) {
    return false;
  }

  winrt::com_ptr<IAudioClient> audio_client;
  HRESULT hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  audio_client.put_void());
  if (FAILED(hr)) {
    SetError(error, L"Could not activate IAudioClient: " + HResultText(hr) +
                        L".");
    return false;
  }

  WaveFormatHolder mix_format;
  hr = audio_client->GetMixFormat(&mix_format.value);
  if (FAILED(hr) || mix_format.value == nullptr) {
    SetError(error, L"IAudioClient::GetMixFormat failed: " + HResultText(hr) +
                        L".");
    return false;
  }

  result->format = StreamFormatFromWaveFormat(*mix_format.value);
  if (!result->format.IsValid()) {
    SetError(error, L"Default render endpoint returned an unsupported PCM mix format.");
    return false;
  }

  DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  constexpr REFERENCE_TIME kBufferDuration = 10000000;
  hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                kBufferDuration, 0, mix_format.value, nullptr);

  EventHandle capture_event;
  if (SUCCEEDED(hr)) {
    capture_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (capture_event.value == nullptr) {
      SetError(error, L"Could not create WASAPI loopback event.");
      return false;
    }

    hr = audio_client->SetEventHandle(capture_event.value);
    if (FAILED(hr)) {
      SetError(error, L"IAudioClient::SetEventHandle failed: " +
                          HResultText(hr) + L".");
      return false;
    }

    result->used_event_callback = true;
  } else {
    stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                  kBufferDuration, 0, mix_format.value, nullptr);
    if (FAILED(hr)) {
      SetError(error, L"IAudioClient::Initialize loopback failed: " +
                          HResultText(hr) + L".");
      return false;
    }
  }

  winrt::com_ptr<IAudioCaptureClient> capture_client;
  hr = audio_client->GetService(__uuidof(IAudioCaptureClient),
                                capture_client.put_void());
  if (FAILED(hr)) {
    SetError(error, L"Could not get IAudioCaptureClient: " + HResultText(hr) +
                        L".");
    return false;
  }

  hr = audio_client->Start();
  if (FAILED(hr)) {
    SetError(error, L"IAudioClient::Start failed: " + HResultText(hr) + L".");
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + duration;
  bool ok = true;
  while (std::chrono::steady_clock::now() < deadline) {
    if (result->used_event_callback) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      const auto wait_ms = static_cast<DWORD>(std::clamp<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(),
          1, 100));
      WaitForSingleObject(capture_event.value, wait_ms);
    } else {
      Sleep(10);
    }

    if (!DrainLoopbackPackets(capture_client.get(), result, sink, error)) {
      ok = false;
      break;
    }
  }

  if (ok) {
    ok = DrainLoopbackPackets(capture_client.get(), result, sink, error);
  }

  audio_client->Stop();
  return ok;
}

}  // namespace olouie::audio
