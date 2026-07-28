#include "performance/CapturePerformance.h"

namespace olouie::performance {

bool IsValidCapturePerformanceMode(CapturePerformanceMode mode) noexcept {
  switch (mode) {
    case CapturePerformanceMode::Balanced:
    case CapturePerformanceMode::CaptureFirst:
      return true;
  }
  return false;
}

const wchar_t* CapturePerformanceModeName(
    CapturePerformanceMode mode) noexcept {
  switch (mode) {
    case CapturePerformanceMode::Balanced:
      return L"balanced";
    case CapturePerformanceMode::CaptureFirst:
      return L"capture first";
  }
  return L"unknown";
}

}  // namespace olouie::performance
