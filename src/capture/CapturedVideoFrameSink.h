#pragma once

#include <string>

#include "capture/VideoFrameQueue.h"

namespace olouie::capture {

enum class CapturedVideoFrameSinkStatus {
  Success,
  Dropped,
  DroppedAndQueued,
  InvalidFrame,
  InvalidConfig,
  SinkError,
};

struct CapturedVideoFrameSinkResult {
  CapturedVideoFrameSinkStatus status =
      CapturedVideoFrameSinkStatus::SinkError;
  std::wstring message;
  uint32_t dropped_frame_count = 0;

  bool Succeeded() const noexcept;
  bool Accepted() const noexcept;
  bool Dropped() const noexcept;
};

class ICapturedVideoFrameSink {
 public:
  virtual ~ICapturedVideoFrameSink() = default;

  virtual CapturedVideoFrameSinkResult OnCapturedVideoFrame(
      OwnedVideoFrame frame) = 0;
};

CapturedVideoFrameSinkResult DispatchCapturedVideoFrame(
    ICapturedVideoFrameSink* sink,
    OwnedVideoFrame frame);

const wchar_t* CapturedVideoFrameSinkStatusName(
    CapturedVideoFrameSinkStatus status) noexcept;

}  // namespace olouie::capture
