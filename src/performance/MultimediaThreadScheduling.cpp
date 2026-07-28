#include "performance/MultimediaThreadScheduling.h"

#include <avrt.h>

#include <utility>

namespace olouie::performance {
namespace {

AVRT_PRIORITY AvrtPriority(
    MultimediaThreadRelativePriority priority) noexcept {
  return priority == MultimediaThreadRelativePriority::High
             ? AVRT_PRIORITY_HIGH
             : AVRT_PRIORITY_NORMAL;
}

std::wstring Win32FailureMessage(std::wstring operation,
                                 DWORD error) {
  return std::move(operation) + L" failed with Win32 error " +
         std::to_wstring(error) + L".";
}

}  // namespace

bool MultimediaThreadSchedulingPlan::IsValid() const noexcept {
  if (!IsValidCapturePerformanceMode(mode) || task_name.empty() ||
      changes_process_priority) {
    return false;
  }
  switch (workload) {
    case MultimediaThreadWorkload::Recorder:
    case MultimediaThreadWorkload::Capture:
    case MultimediaThreadWorkload::AudioCapture:
    case MultimediaThreadWorkload::VideoEncode:
      break;
    default:
      return false;
  }
  switch (relative_priority) {
    case MultimediaThreadRelativePriority::Normal:
    case MultimediaThreadRelativePriority::High:
      return true;
  }
  return false;
}

bool MultimediaThreadSchedulingSnapshot::Succeeded() const noexcept {
  return attempted && registered && priority_applied;
}

MultimediaThreadSchedulingPlan BuildMultimediaThreadSchedulingPlan(
    CapturePerformanceMode mode,
    MultimediaThreadWorkload workload) {
  MultimediaThreadSchedulingPlan plan;
  plan.mode = mode;
  plan.workload = workload;
  plan.task_name = workload == MultimediaThreadWorkload::AudioCapture
                       ? L"Audio"
                       : L"Capture";
  plan.relative_priority =
      mode == CapturePerformanceMode::CaptureFirst
          ? MultimediaThreadRelativePriority::High
          : MultimediaThreadRelativePriority::Normal;
  plan.changes_process_priority = false;
  return plan;
}

MultimediaThreadRegistration::~MultimediaThreadRegistration() {
  Reset();
}

MultimediaThreadSchedulingSnapshot MultimediaThreadRegistration::Register(
    const MultimediaThreadSchedulingPlan& plan) {
  Reset();
  snapshot_.plan = plan;
  snapshot_.attempted = true;
  if (!plan.IsValid()) {
    snapshot_.message = L"Multimedia scheduling plan is invalid.";
    snapshot_.win32_error = ERROR_INVALID_PARAMETER;
    return snapshot_;
  }

  DWORD task_index = 0;
  SetLastError(ERROR_SUCCESS);
  task_handle_ =
      AvSetMmThreadCharacteristicsW(plan.task_name.c_str(), &task_index);
  if (task_handle_ == nullptr) {
    snapshot_.win32_error = GetLastError();
    snapshot_.message = Win32FailureMessage(
        L"AvSetMmThreadCharacteristicsW", snapshot_.win32_error);
    return snapshot_;
  }
  snapshot_.registered = true;
  snapshot_.task_index = task_index;

  SetLastError(ERROR_SUCCESS);
  if (!AvSetMmThreadPriority(task_handle_,
                             AvrtPriority(plan.relative_priority))) {
    snapshot_.win32_error = GetLastError();
    snapshot_.message = Win32FailureMessage(
        L"AvSetMmThreadPriority", snapshot_.win32_error);
    return snapshot_;
  }

  snapshot_.priority_applied = true;
  snapshot_.message = L"MMCSS scheduling applied.";
  return snapshot_;
}

void MultimediaThreadRegistration::Reset() noexcept {
  if (task_handle_ != nullptr) {
    (void)AvRevertMmThreadCharacteristics(task_handle_);
    task_handle_ = nullptr;
  }
  snapshot_ = {};
}

const MultimediaThreadSchedulingSnapshot&
MultimediaThreadRegistration::snapshot() const noexcept {
  return snapshot_;
}

const wchar_t* MultimediaThreadWorkloadName(
    MultimediaThreadWorkload workload) noexcept {
  switch (workload) {
    case MultimediaThreadWorkload::Recorder:
      return L"recorder";
    case MultimediaThreadWorkload::Capture:
      return L"capture";
    case MultimediaThreadWorkload::AudioCapture:
      return L"audio capture";
    case MultimediaThreadWorkload::VideoEncode:
      return L"video encode";
  }
  return L"unknown";
}

const wchar_t* MultimediaThreadRelativePriorityName(
    MultimediaThreadRelativePriority priority) noexcept {
  switch (priority) {
    case MultimediaThreadRelativePriority::Normal:
      return L"normal";
    case MultimediaThreadRelativePriority::High:
      return L"high";
  }
  return L"unknown";
}

}  // namespace olouie::performance
