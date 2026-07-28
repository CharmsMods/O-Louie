#include "capture/CapturedVideoFrameSink.h"

#include <string>
#include <utility>

namespace olouie::capture {
namespace {

CapturedVideoFrameSinkResult Result(CapturedVideoFrameSinkStatus status,
                                    std::wstring message) {
  CapturedVideoFrameSinkResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

}  // namespace

bool CapturedVideoFrameSinkResult::Succeeded() const noexcept {
  return status == CapturedVideoFrameSinkStatus::Success ||
         status == CapturedVideoFrameSinkStatus::Dropped ||
         status == CapturedVideoFrameSinkStatus::DroppedAndQueued;
}

bool CapturedVideoFrameSinkResult::Accepted() const noexcept {
  return status == CapturedVideoFrameSinkStatus::Success ||
         status == CapturedVideoFrameSinkStatus::DroppedAndQueued;
}

bool CapturedVideoFrameSinkResult::Dropped() const noexcept {
  return status == CapturedVideoFrameSinkStatus::Dropped ||
         status == CapturedVideoFrameSinkStatus::DroppedAndQueued;
}

CapturedVideoFrameSinkResult DispatchCapturedVideoFrame(
    ICapturedVideoFrameSink* sink,
    OwnedVideoFrame frame) {
  if (!frame.IsValid()) {
    return Result(CapturedVideoFrameSinkStatus::InvalidFrame,
                  L"Captured video frame is invalid.");
  }

  if (sink == nullptr) {
    return Result(CapturedVideoFrameSinkStatus::Success, L"");
  }

  return sink->OnCapturedVideoFrame(std::move(frame));
}

const wchar_t* CapturedVideoFrameSinkStatusName(
    CapturedVideoFrameSinkStatus status) noexcept {
  switch (status) {
    case CapturedVideoFrameSinkStatus::Success:
      return L"success";
    case CapturedVideoFrameSinkStatus::Dropped:
      return L"dropped";
    case CapturedVideoFrameSinkStatus::DroppedAndQueued:
      return L"dropped and queued";
    case CapturedVideoFrameSinkStatus::InvalidFrame:
      return L"invalid frame";
    case CapturedVideoFrameSinkStatus::InvalidConfig:
      return L"invalid config";
    case CapturedVideoFrameSinkStatus::SinkError:
      return L"sink error";
  }

  return L"unknown";
}

}  // namespace olouie::capture
