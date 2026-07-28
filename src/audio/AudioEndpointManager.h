#pragma once

#include <string>
#include <vector>

namespace olouie::audio {

enum class AudioEndpointFlow {
  Render,
  Capture,
};

struct AudioEndpointInfo {
  AudioEndpointFlow flow = AudioEndpointFlow::Render;
  std::wstring id;
  std::wstring name;
  bool is_default = false;
};

const wchar_t* FlowName(AudioEndpointFlow flow) noexcept;
bool EnumerateActiveAudioEndpoints(AudioEndpointFlow flow,
                                   std::vector<AudioEndpointInfo>* endpoints,
                                   std::wstring* error);
bool TryGetDefaultAudioEndpoint(AudioEndpointFlow flow,
                                AudioEndpointInfo* endpoint,
                                std::wstring* error);

}  // namespace olouie::audio
