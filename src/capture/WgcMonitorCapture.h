#pragma once

#include <windows.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "capture/VideoFrameQueue.h"

namespace olouie::capture {

class ICapturedVideoFrameSink;

struct WgcSmokeResult {
  bool supported = false;
  uint32_t frame_count = 0;
  int64_t first_timestamp_ticks = 0;
  int64_t last_timestamp_ticks = 0;
  std::wstring error;
};

using WgcCopiedFrame = OwnedVideoFrame;

struct WgcFrameCopySmokeResult {
  bool supported = false;
  uint32_t frame_count = 0;
  uint32_t copied_frame_count = 0;
  uint32_t dropped_frame_count = 0;
  int64_t first_timestamp_ticks = 0;
  int64_t last_timestamp_ticks = 0;
  std::vector<WgcCopiedFrame> frames;
  std::wstring error;
};

bool IsWgcSupported();
WgcSmokeResult RunWgcMonitorSmoke(HMONITOR monitor, ID3D11Device* device,
                                  std::chrono::milliseconds duration);
WgcFrameCopySmokeResult RunWgcMonitorFrameCopySmoke(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames);
WgcFrameCopySmokeResult RunWgcMonitorFrameCopySmoke(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames,
    ICapturedVideoFrameSink* sink);

}  // namespace olouie::capture
