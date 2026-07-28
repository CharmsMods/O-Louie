#include "audio/AudioEndpointManager.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkeydef.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winrt/base.h>

#include <algorithm>
#include <string>

namespace olouie::audio {
namespace {

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

  PropVariantHolder() {
    PropVariantInit(&value);
  }

  ~PropVariantHolder() {
    PropVariantClear(&value);
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

EDataFlow ToDataFlow(AudioEndpointFlow flow) {
  return flow == AudioEndpointFlow::Render ? eRender : eCapture;
}

bool CreateEnumerator(winrt::com_ptr<IMMDeviceEnumerator>* enumerator,
                      std::wstring* error) {
  if (enumerator == nullptr) {
    SetError(error, L"Audio endpoint enumeration needs an output destination.");
    return false;
  }

  const HRESULT result =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       IID_PPV_ARGS(enumerator->put()));
  if (FAILED(result)) {
    SetError(error, L"Could not create MMDeviceEnumerator: " +
                        HResultText(result) + L".");
    return false;
  }

  return true;
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

bool ReadEndpointInfo(IMMDevice* device, AudioEndpointFlow flow,
                      const std::wstring& default_id,
                      AudioEndpointInfo* endpoint, std::wstring* error) {
  if (device == nullptr || endpoint == nullptr) {
    SetError(error, L"Audio endpoint read needs a valid device and output.");
    return false;
  }

  AudioEndpointInfo info;
  info.flow = flow;
  info.id = ReadEndpointId(device);
  if (info.id.empty()) {
    SetError(error, L"Audio endpoint has no stable id.");
    return false;
  }

  info.name = ReadEndpointName(device);
  if (info.name.empty()) {
    info.name = info.id;
  }

  info.is_default = !default_id.empty() && info.id == default_id;
  *endpoint = std::move(info);
  return true;
}

std::wstring ReadDefaultEndpointId(IMMDeviceEnumerator* enumerator,
                                   AudioEndpointFlow flow) {
  if (enumerator == nullptr) {
    return {};
  }

  winrt::com_ptr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(ToDataFlow(flow), eConsole,
                                                 device.put()))) {
    return {};
  }

  return ReadEndpointId(device.get());
}

}  // namespace

const wchar_t* FlowName(AudioEndpointFlow flow) noexcept {
  return flow == AudioEndpointFlow::Render ? L"render" : L"capture";
}

bool EnumerateActiveAudioEndpoints(AudioEndpointFlow flow,
                                   std::vector<AudioEndpointInfo>* endpoints,
                                   std::wstring* error) {
  if (endpoints == nullptr) {
    SetError(error, L"Audio endpoint enumeration needs an output destination.");
    return false;
  }

  endpoints->clear();

  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  if (!CreateEnumerator(&enumerator, error)) {
    return false;
  }

  const std::wstring default_id = ReadDefaultEndpointId(enumerator.get(), flow);

  winrt::com_ptr<IMMDeviceCollection> collection;
  const HRESULT enum_result = enumerator->EnumAudioEndpoints(
      ToDataFlow(flow), DEVICE_STATE_ACTIVE, collection.put());
  if (FAILED(enum_result)) {
    SetError(error, L"Could not enumerate active audio endpoints: " +
                        HResultText(enum_result) + L".");
    return false;
  }

  UINT count = 0;
  if (FAILED(collection->GetCount(&count))) {
    SetError(error, L"Could not read audio endpoint count.");
    return false;
  }

  endpoints->reserve(count);
  for (UINT index = 0; index < count; ++index) {
    winrt::com_ptr<IMMDevice> device;
    if (FAILED(collection->Item(index, device.put()))) {
      continue;
    }

    AudioEndpointInfo info;
    if (ReadEndpointInfo(device.get(), flow, default_id, &info, nullptr)) {
      endpoints->push_back(std::move(info));
    }
  }

  std::sort(endpoints->begin(), endpoints->end(),
            [](const AudioEndpointInfo& left,
               const AudioEndpointInfo& right) {
              if (left.is_default != right.is_default) {
                return left.is_default;
              }
              return left.name < right.name;
            });

  return true;
}

bool TryGetDefaultAudioEndpoint(AudioEndpointFlow flow,
                                AudioEndpointInfo* endpoint,
                                std::wstring* error) {
  if (endpoint == nullptr) {
    SetError(error, L"Default audio endpoint discovery needs an output destination.");
    return false;
  }

  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  if (!CreateEnumerator(&enumerator, error)) {
    return false;
  }

  winrt::com_ptr<IMMDevice> device;
  const HRESULT result = enumerator->GetDefaultAudioEndpoint(
      ToDataFlow(flow), eConsole, device.put());
  if (FAILED(result)) {
    SetError(error, std::wstring(L"No default ") + FlowName(flow) +
                        L" audio endpoint is available: " +
                        HResultText(result) + L".");
    return false;
  }

  return ReadEndpointInfo(device.get(), flow, ReadEndpointId(device.get()),
                          endpoint, error);
}

}  // namespace olouie::audio
