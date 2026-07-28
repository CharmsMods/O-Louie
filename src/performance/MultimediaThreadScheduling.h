#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "performance/CapturePerformance.h"

namespace olouie::performance {

enum class MultimediaThreadWorkload {
  Recorder,
  Capture,
  AudioCapture,
  VideoEncode,
};

enum class MultimediaThreadRelativePriority {
  Normal,
  High,
};

struct MultimediaThreadSchedulingPlan {
  CapturePerformanceMode mode = CapturePerformanceMode::Balanced;
  MultimediaThreadWorkload workload =
      MultimediaThreadWorkload::Recorder;
  std::wstring task_name;
  MultimediaThreadRelativePriority relative_priority =
      MultimediaThreadRelativePriority::Normal;
  bool changes_process_priority = false;

  bool IsValid() const noexcept;
};

struct MultimediaThreadSchedulingSnapshot {
  MultimediaThreadSchedulingPlan plan;
  bool attempted = false;
  bool registered = false;
  bool priority_applied = false;
  uint32_t task_index = 0;
  uint32_t win32_error = ERROR_SUCCESS;
  std::wstring message;

  bool Succeeded() const noexcept;
};

MultimediaThreadSchedulingPlan BuildMultimediaThreadSchedulingPlan(
    CapturePerformanceMode mode,
    MultimediaThreadWorkload workload);

class MultimediaThreadRegistration final {
 public:
  MultimediaThreadRegistration() = default;
  ~MultimediaThreadRegistration();

  MultimediaThreadRegistration(const MultimediaThreadRegistration&) = delete;
  MultimediaThreadRegistration& operator=(
      const MultimediaThreadRegistration&) = delete;

  MultimediaThreadSchedulingSnapshot Register(
      const MultimediaThreadSchedulingPlan& plan);
  void Reset() noexcept;

  const MultimediaThreadSchedulingSnapshot& snapshot() const noexcept;

 private:
  HANDLE task_handle_ = nullptr;
  MultimediaThreadSchedulingSnapshot snapshot_;
};

const wchar_t* MultimediaThreadWorkloadName(
    MultimediaThreadWorkload workload) noexcept;
const wchar_t* MultimediaThreadRelativePriorityName(
    MultimediaThreadRelativePriority priority) noexcept;

}  // namespace olouie::performance
