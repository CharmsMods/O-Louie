#pragma once

namespace olouie::performance {

enum class CapturePerformanceMode {
  Balanced,
  CaptureFirst,
};

bool IsValidCapturePerformanceMode(CapturePerformanceMode mode) noexcept;
const wchar_t* CapturePerformanceModeName(
    CapturePerformanceMode mode) noexcept;

}  // namespace olouie::performance
