#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "capture/BgraTexturePool.h"
#include "capture/VideoFrameCadence.h"
#include "graphics/D3D11DeviceFault.h"
#include "performance/MultimediaThreadScheduling.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace olouie::capture {

class ICapturedVideoFrameSink;

enum class WgcMonitorCaptureSessionStatus {
  Success,
  InvalidConfig,
  Unsupported,
  StartFailed,
  StopFailed,
};

enum class WgcMonitorCaptureFaultKind {
  None,
  MonitorResized,
  MonitorDisconnected,
  D3D11DeviceLost,
  InvalidFrame,
  FrameCopyFailed,
  SinkFailed,
  CallbackFailed,
};

struct WgcMonitorCaptureFault {
  WgcMonitorCaptureFaultKind kind = WgcMonitorCaptureFaultKind::None;
  std::wstring message;
  uint32_t expected_width = 0;
  uint32_t expected_height = 0;
  uint32_t observed_width = 0;
  uint32_t observed_height = 0;
  graphics::D3D11DeviceFault device_fault;

  bool Failed() const noexcept;
};

class WgcMonitorCaptureFaultLatch final {
 public:
  void Reset();
  bool Report(WgcMonitorCaptureFault fault);
  WgcMonitorCaptureFault Snapshot() const;

 private:
  mutable std::mutex mutex_;
  WgcMonitorCaptureFault fault_;
};

struct WgcMonitorCaptureSessionOptions {
  bool capture_cursor = true;
  VideoFrameCadenceConfig frame_cadence;
  uint32_t owned_texture_pool_capacity = 0;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
};

struct WgcMonitorCaptureSessionResult {
  WgcMonitorCaptureSessionStatus status =
      WgcMonitorCaptureSessionStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct WgcMonitorCaptureSessionSnapshot {
  bool supported = false;
  bool running = false;
  uint64_t received_frame_count = 0;
  uint64_t accepted_frame_count = 0;
  uint64_t rate_limited_frame_count = 0;
  uint64_t dropped_frame_count = 0;
  uint64_t texture_pool_exhausted_frame_count = 0;
  uint64_t texture_copy_submission_count = 0;
  uint64_t last_texture_copy_submission_latency_ns = 0;
  uint64_t maximum_texture_copy_submission_latency_ns = 0;
  uint64_t total_texture_copy_submission_latency_ns = 0;
  performance::MultimediaThreadSchedulingSnapshot scheduling;
  BgraTexturePoolStats texture_pool;
  int64_t first_timestamp_ticks = 0;
  int64_t last_timestamp_ticks = 0;
  WgcMonitorCaptureFault fault;
  std::wstring error;

  bool Failed() const noexcept;
};

class WgcMonitorCaptureSession final {
 public:
  WgcMonitorCaptureSession();
  ~WgcMonitorCaptureSession();

  WgcMonitorCaptureSession(const WgcMonitorCaptureSession&) = delete;
  WgcMonitorCaptureSession& operator=(const WgcMonitorCaptureSession&) =
      delete;

  WgcMonitorCaptureSessionResult Start(
      HMONITOR monitor,
      ID3D11Device* device,
      ID3D11DeviceContext* context,
      const WgcMonitorCaptureSessionOptions& options,
      ICapturedVideoFrameSink* sink);
  WgcMonitorCaptureSessionResult Stop();

  bool IsRunning() const noexcept;
  WgcMonitorCaptureSessionSnapshot Snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const wchar_t* WgcMonitorCaptureSessionStatusName(
    WgcMonitorCaptureSessionStatus status) noexcept;
const wchar_t* WgcMonitorCaptureFaultKindName(
    WgcMonitorCaptureFaultKind kind) noexcept;
bool IsSelectedMonitorTopologyFault(
    WgcMonitorCaptureFaultKind kind) noexcept;
bool IsWgcD3D11DeviceFault(
    WgcMonitorCaptureFaultKind kind) noexcept;

}  // namespace olouie::capture
