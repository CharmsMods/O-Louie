#include "capture/WgcMonitorCaptureSession.h"

#include "capture/CapturedVideoFrameSink.h"
#include "capture/WgcMonitorCapture.h"

#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <utility>

namespace olouie::capture {
namespace {

using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Direct3DDxgiInterfaceAccess =
    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

WgcMonitorCaptureSessionResult Result(
    WgcMonitorCaptureSessionStatus status,
    std::wstring message) {
  WgcMonitorCaptureSessionResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

void UpdateAtomicMaximum(std::atomic_uint64_t* destination,
                         uint64_t value) noexcept {
  auto current = destination->load();
  while (current < value &&
         !destination->compare_exchange_weak(current, value)) {
  }
}

IDirect3DDevice CreateDirect3DDevice(ID3D11Device* device) {
  winrt::com_ptr<IDXGIDevice> dxgi_device;
  winrt::check_hresult(device->QueryInterface(dxgi_device.put()));

  winrt::com_ptr<::IInspectable> inspectable;
  winrt::check_hresult(
      CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(),
                                           inspectable.put()));
  return inspectable.as<IDirect3DDevice>();
}

GraphicsCaptureItem CreateMonitorItem(HMONITOR monitor) {
  auto interop =
      winrt::get_activation_factory<GraphicsCaptureItem,
                                    IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(interop->CreateForMonitor(
      monitor, __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
      winrt::put_abi(item)));
  return item;
}

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    const IDirect3DSurface& surface) {
  auto access = surface.as<Direct3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  winrt::check_hresult(
      access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
  return texture;
}

HRESULT CreateCopiedBgraTexture(ID3D11Device* device,
                                uint32_t width,
                                uint32_t height,
                                ID3D11Texture2D** texture) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  return device->CreateTexture2D(&desc, nullptr, texture);
}

}  // namespace

struct WgcMonitorCaptureSession::Impl {
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  IDirect3DDevice direct3d_device{nullptr};
  GraphicsCaptureItem item{nullptr};
  Direct3D11CaptureFramePool frame_pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  winrt::event_token item_closed_token{};
  winrt::event_token frame_arrived_token{};
  bool item_closed_registered = false;
  bool frame_arrived_registered = false;
  ICapturedVideoFrameSink* sink = nullptr;
  HMONITOR monitor = nullptr;
  std::wstring monitor_device_name;
  uint32_t expected_monitor_width = 0;
  uint32_t expected_monitor_height = 0;
  uint32_t expected_width = 0;
  uint32_t expected_height = 0;

  std::atomic_bool running{false};
  std::atomic_bool callback_failed{false};
  std::atomic_uint64_t received_frame_count{0};
  std::atomic_uint64_t accepted_frame_count{0};
  std::atomic_uint64_t rate_limited_frame_count{0};
  std::atomic_uint64_t dropped_frame_count{0};
  std::atomic_uint64_t texture_pool_exhausted_frame_count{0};
  std::atomic_uint64_t texture_copy_submission_count{0};
  std::atomic_uint64_t last_texture_copy_submission_latency_ns{0};
  std::atomic_uint64_t maximum_texture_copy_submission_latency_ns{0};
  std::atomic_uint64_t total_texture_copy_submission_latency_ns{0};
  std::atomic_int64_t first_timestamp_ticks{0};
  std::atomic_int64_t last_timestamp_ticks{0};
  std::atomic_uint32_t active_callback_count{0};
  mutable std::mutex callback_mutex;
  std::condition_variable callback_finished;
  WgcMonitorCaptureFaultLatch fault_latch;
  VideoFrameCadence frame_cadence;
  bool frame_cadence_enabled = false;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
  mutable std::mutex scheduling_mutex;
  performance::MultimediaThreadSchedulingSnapshot scheduling;
  std::mutex frame_cadence_mutex;
  BgraTexturePool texture_pool;
  BgraTexturePoolStats last_texture_pool_stats;

  struct CallbackGuard {
    Impl* self;

    explicit CallbackGuard(Impl* implementation) : self(implementation) {
      self->active_callback_count.fetch_add(1);
    }

    ~CallbackGuard() {
      if (self->active_callback_count.fetch_sub(1) == 1) {
        self->callback_finished.notify_all();
      }
    }
  };

  void SetCallbackFault(WgcMonitorCaptureFault fault) {
    if (fault_latch.Report(std::move(fault))) {
      callback_failed.store(true);
    }
  }

  void SetD3D11DeviceFault(graphics::D3D11DeviceFault device_fault) {
    if (!device_fault.Failed()) {
      return;
    }
    WgcMonitorCaptureFault fault;
    fault.kind = WgcMonitorCaptureFaultKind::D3D11DeviceLost;
    fault.message = device_fault.message;
    fault.expected_width = expected_width;
    fault.expected_height = expected_height;
    fault.device_fault = std::move(device_fault);
    SetCallbackFault(std::move(fault));
  }

  void CaptureItemClosed() {
    CallbackGuard guard(this);
    if (!running.load()) {
      return;
    }
    SetCallbackFault(WgcMonitorCaptureFault{
        WgcMonitorCaptureFaultKind::MonitorDisconnected,
        L"The selected monitor was disconnected or became unavailable. "
        L"Recording stopped without switching to another monitor.",
        expected_width,
        expected_height});
  }

  void PollSelectedMonitorTopology() {
    if (!running.load() || callback_failed.load() || monitor == nullptr) {
      return;
    }

    MONITORINFOEXW current{};
    current.cbSize = sizeof(current);
    if (!GetMonitorInfoW(monitor, &current) ||
        monitor_device_name != current.szDevice) {
      SetCallbackFault(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::MonitorDisconnected,
          L"The selected monitor was disconnected or became unavailable. "
          L"Recording stopped without switching to another monitor.",
          expected_monitor_width,
          expected_monitor_height});
      return;
    }

    const LONG current_width = current.rcMonitor.right - current.rcMonitor.left;
    const LONG current_height =
        current.rcMonitor.bottom - current.rcMonitor.top;
    if (current_width <= 0 || current_height <= 0) {
      SetCallbackFault(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::MonitorDisconnected,
          L"The selected monitor no longer has a valid display area. "
          L"Recording stopped without switching to another monitor.",
          expected_monitor_width,
          expected_monitor_height});
      return;
    }

    const auto width = static_cast<uint32_t>(current_width);
    const auto height = static_cast<uint32_t>(current_height);
    if (width != expected_monitor_width ||
        height != expected_monitor_height) {
      SetCallbackFault(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::MonitorResized,
          L"The selected monitor resized from " +
              std::to_wstring(expected_monitor_width) + L"x" +
              std::to_wstring(expected_monitor_height) + L" to " +
              std::to_wstring(width) + L"x" + std::to_wstring(height) +
              L". Recording stopped to keep one fixed video size.",
          expected_monitor_width,
          expected_monitor_height,
          width,
          height});
    }
  }

  void PollD3D11Device() {
    if (!running.load() || callback_failed.load() || device == nullptr) {
      return;
    }
    SetD3D11DeviceFault(graphics::InspectD3D11DeviceFault(
        device.get(), S_OK, L"WGC monitor capture"));
  }

  void FrameArrived(const Direct3D11CaptureFramePool& sender) {
    CallbackGuard guard(this);
    if (!running.load()) {
      return;
    }
    performance::MultimediaThreadRegistration thread_scheduling;
    const auto scheduling_snapshot = thread_scheduling.Register(
        performance::BuildMultimediaThreadSchedulingPlan(
            performance_mode,
            performance::MultimediaThreadWorkload::Capture));
    {
      std::lock_guard lock(scheduling_mutex);
      scheduling = scheduling_snapshot;
    }

    for (;;) {
      try {
        Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
        if (!frame) {
          break;
        }

        int64_t timestamp = frame.SystemRelativeTime().count();
        const auto observe_frame = [this](int64_t observed_timestamp) {
          const uint64_t frame_index = received_frame_count.fetch_add(1);
          if (frame_index == 0) {
            first_timestamp_ticks.store(observed_timestamp);
          }
          last_timestamp_ticks.store(observed_timestamp);
        };
        observe_frame(timestamp);

        // When a callback is delayed, only the freshest pooled frame is useful
        // for the next output slot. Older pooled frames would create a catch-up
        // burst and additional full-size GPU copies.
        if (frame_cadence_enabled) {
          for (;;) {
            Direct3D11CaptureFrame newer = sender.TryGetNextFrame();
            if (!newer) {
              break;
            }
            rate_limited_frame_count.fetch_add(1);
            frame = std::move(newer);
            timestamp = frame.SystemRelativeTime().count();
            observe_frame(timestamp);
          }
        }

        if (!running.load() || callback_failed.load()) {
          continue;
        }

        if (frame_cadence_enabled) {
          VideoFrameCadenceDecision cadence_decision;
          {
            std::lock_guard lock(frame_cadence_mutex);
            cadence_decision = frame_cadence.Evaluate(timestamp);
          }
          if (cadence_decision == VideoFrameCadenceDecision::RateLimited) {
            rate_limited_frame_count.fetch_add(1);
            continue;
          }
          if (cadence_decision != VideoFrameCadenceDecision::Accepted) {
            SetCallbackFault(WgcMonitorCaptureFault{
                WgcMonitorCaptureFaultKind::InvalidFrame,
                L"WGC capture received an invalid frame timestamp for "
                L"configured cadence pacing.",
                expected_width,
                expected_height});
            continue;
          }
        }

        const auto content_size = frame.ContentSize();
        if (content_size.Width <= 0 || content_size.Height <= 0) {
          SetCallbackFault(WgcMonitorCaptureFault{
              WgcMonitorCaptureFaultKind::InvalidFrame,
              L"WGC capture received an invalid frame size.",
              expected_width,
              expected_height});
          continue;
        }

        const auto width = static_cast<uint32_t>(content_size.Width);
        const auto height = static_cast<uint32_t>(content_size.Height);
        if (width != expected_width || height != expected_height) {
          SetCallbackFault(WgcMonitorCaptureFault{
              WgcMonitorCaptureFaultKind::MonitorResized,
              L"The selected monitor resized from " +
                  std::to_wstring(expected_width) + L"x" +
                  std::to_wstring(expected_height) + L" to " +
                  std::to_wstring(width) + L"x" + std::to_wstring(height) +
                  L". Recording stopped to keep one fixed video size.",
              expected_width,
              expected_height,
              width,
              height});
          continue;
        }

        auto source_texture = GetTextureFromSurface(frame.Surface());
        D3D11_TEXTURE2D_DESC source_desc{};
        source_texture->GetDesc(&source_desc);
        if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
          SetCallbackFault(WgcMonitorCaptureFault{
              WgcMonitorCaptureFaultKind::InvalidFrame,
              L"WGC capture received a non-BGRA frame.",
              expected_width,
              expected_height,
              width,
              height});
          continue;
        }

        winrt::com_ptr<ID3D11Texture2D> copied_texture;
        BgraTexturePoolLease texture_pool_lease;
        HRESULT create_result = S_OK;
        graphics::D3D11DeviceFault texture_device_fault;
        std::wstring texture_error;
        if (texture_pool.IsInitialized()) {
          auto acquired = texture_pool.Acquire();
          if (acquired.status == BgraTexturePoolAcquireStatus::Exhausted) {
            texture_pool_exhausted_frame_count.fetch_add(1);
            dropped_frame_count.fetch_add(1);
            continue;
          }
          if (!acquired.Succeeded()) {
            create_result = acquired.hresult;
            texture_device_fault = std::move(acquired.device_fault);
            texture_error = std::move(acquired.message);
          } else {
            copied_texture.copy_from(acquired.lease.get());
            texture_pool_lease = std::move(acquired.lease);
          }
        } else {
          create_result = CreateCopiedBgraTexture(
              device.get(), width, height, copied_texture.put());
        }
        if (copied_texture == nullptr || FAILED(create_result)) {
          auto device_fault = graphics::InspectD3D11DeviceFault(
              device.get(), create_result,
              L"WGC app-owned texture creation");
          if (texture_device_fault.Failed()) {
            device_fault = std::move(texture_device_fault);
          }
          if (device_fault.Failed()) {
            SetD3D11DeviceFault(std::move(device_fault));
            continue;
          }
          SetCallbackFault(WgcMonitorCaptureFault{
              WgcMonitorCaptureFaultKind::FrameCopyFailed,
              texture_error.empty()
                  ? L"Could not create an app-owned WGC texture (" +
                        HResultToHex(create_result) + L")."
                  : std::move(texture_error),
              expected_width,
              expected_height,
              width,
              height});
          continue;
        }

        D3D11_BOX source_box{};
        source_box.right = width;
        source_box.bottom = height;
        source_box.back = 1;
        const auto copy_started = std::chrono::steady_clock::now();
        context->CopySubresourceRegion(copied_texture.get(), 0, 0, 0, 0,
                                       source_texture.get(), 0, &source_box);
        const auto copy_finished = std::chrono::steady_clock::now();
        const auto copy_latency_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                copy_finished - copy_started)
                .count());
        texture_copy_submission_count.fetch_add(1);
        last_texture_copy_submission_latency_ns.store(copy_latency_ns);
        total_texture_copy_submission_latency_ns.fetch_add(copy_latency_ns);
        UpdateAtomicMaximum(&maximum_texture_copy_submission_latency_ns,
                            copy_latency_ns);
        auto copy_device_fault = graphics::InspectD3D11DeviceFault(
            device.get(), S_OK, L"WGC texture copy");
        if (copy_device_fault.Failed()) {
          SetD3D11DeviceFault(std::move(copy_device_fault));
          continue;
        }

        OwnedVideoFrame copied_frame;
        copied_frame.texture_pool_lease = std::move(texture_pool_lease);
        copied_frame.texture = std::move(copied_texture);
        copied_frame.width = width;
        copied_frame.height = height;
        copied_frame.timestamp_ticks = timestamp;

        const auto delivered =
            DispatchCapturedVideoFrame(sink, std::move(copied_frame));
        if (!delivered.Succeeded()) {
          std::wstring message = delivered.message;
          if (message.empty()) {
            message = L"Captured-frame sink failed with status " +
                      std::wstring(CapturedVideoFrameSinkStatusName(
                          delivered.status)) +
                      L".";
          }
          SetCallbackFault(WgcMonitorCaptureFault{
              WgcMonitorCaptureFaultKind::SinkFailed,
              std::move(message),
              expected_width,
              expected_height,
              width,
              height});
          continue;
        }
        if (delivered.Accepted()) {
          accepted_frame_count.fetch_add(1);
        }
        if (delivered.Dropped()) {
          dropped_frame_count.fetch_add(
              delivered.dropped_frame_count == 0
                  ? 1
                  : delivered.dropped_frame_count);
        }
      } catch (const winrt::hresult_error& error) {
        auto device_fault = graphics::InspectD3D11DeviceFault(
            device.get(), error.code(), L"WGC frame callback");
        if (device_fault.Failed()) {
          SetD3D11DeviceFault(std::move(device_fault));
          continue;
        }
        SetCallbackFault(WgcMonitorCaptureFault{
            WgcMonitorCaptureFaultKind::CallbackFailed,
            L"WGC frame callback failed with HRESULT " +
                HResultToHex(error.code()) + L": " +
                std::wstring(error.message()),
            expected_width,
            expected_height});
      } catch (...) {
        SetCallbackFault(WgcMonitorCaptureFault{
            WgcMonitorCaptureFaultKind::CallbackFailed,
            L"WGC frame callback failed unexpectedly.",
            expected_width,
            expected_height});
      }
    }
  }

  void ClearRuntime() {
    session = nullptr;
    frame_pool = nullptr;
    item = nullptr;
    direct3d_device = nullptr;
    context = nullptr;
    device = nullptr;
    sink = nullptr;
    monitor = nullptr;
    monitor_device_name.clear();
    expected_monitor_width = 0;
    expected_monitor_height = 0;
    expected_width = 0;
    expected_height = 0;
    frame_cadence_enabled = false;
    if (texture_pool.IsInitialized()) {
      last_texture_pool_stats = texture_pool.stats();
    }
    texture_pool.Reset();
    item_closed_registered = false;
    frame_arrived_registered = false;
  }
};

bool WgcMonitorCaptureFault::Failed() const noexcept {
  return kind != WgcMonitorCaptureFaultKind::None;
}

void WgcMonitorCaptureFaultLatch::Reset() {
  std::lock_guard lock(mutex_);
  fault_ = {};
}

bool WgcMonitorCaptureFaultLatch::Report(WgcMonitorCaptureFault fault) {
  if (!fault.Failed() || fault.message.empty()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (fault_.Failed()) {
    return false;
  }
  fault_ = std::move(fault);
  return true;
}

WgcMonitorCaptureFault WgcMonitorCaptureFaultLatch::Snapshot() const {
  std::lock_guard lock(mutex_);
  return fault_;
}

bool WgcMonitorCaptureSessionResult::Succeeded() const noexcept {
  return status == WgcMonitorCaptureSessionStatus::Success;
}

bool WgcMonitorCaptureSessionSnapshot::Failed() const noexcept {
  return fault.Failed() || !error.empty();
}

WgcMonitorCaptureSession::WgcMonitorCaptureSession()
    : impl_(std::make_unique<Impl>()) {}

WgcMonitorCaptureSession::~WgcMonitorCaptureSession() {
  (void)Stop();
}

WgcMonitorCaptureSessionResult WgcMonitorCaptureSession::Start(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const WgcMonitorCaptureSessionOptions& options,
    ICapturedVideoFrameSink* sink) {
  if (impl_->running.load()) {
    return Result(WgcMonitorCaptureSessionStatus::InvalidConfig,
                  L"WGC capture is already running.");
  }
  if (monitor == nullptr || device == nullptr || context == nullptr ||
      sink == nullptr) {
    return Result(WgcMonitorCaptureSessionStatus::InvalidConfig,
                  L"WGC capture needs a monitor, D3D11 device/context, and "
                  L"captured-frame sink.");
  }
  const bool cadence_disabled =
      options.frame_cadence.timestamp_frequency == 0 &&
      options.frame_cadence.target_fps_numerator == 0 &&
      options.frame_cadence.target_fps_denominator == 0;
  if (!cadence_disabled && !options.frame_cadence.IsValid()) {
    return Result(WgcMonitorCaptureSessionStatus::InvalidConfig,
                  L"WGC capture frame cadence is invalid.");
  }
  if (!performance::IsValidCapturePerformanceMode(
          options.performance_mode)) {
    return Result(WgcMonitorCaptureSessionStatus::InvalidConfig,
                  L"WGC capture performance mode is invalid.");
  }
  if (!IsWgcSupported()) {
    return Result(WgcMonitorCaptureSessionStatus::Unsupported,
                  L"Windows Graphics Capture is not supported.");
  }

  impl_->callback_failed.store(false);
  impl_->received_frame_count.store(0);
  impl_->accepted_frame_count.store(0);
  impl_->rate_limited_frame_count.store(0);
  impl_->dropped_frame_count.store(0);
  impl_->texture_pool_exhausted_frame_count.store(0);
  impl_->texture_copy_submission_count.store(0);
  impl_->last_texture_copy_submission_latency_ns.store(0);
  impl_->maximum_texture_copy_submission_latency_ns.store(0);
  impl_->total_texture_copy_submission_latency_ns.store(0);
  impl_->first_timestamp_ticks.store(0);
  impl_->last_timestamp_ticks.store(0);
  impl_->fault_latch.Reset();
  impl_->last_texture_pool_stats = {};
  impl_->performance_mode = options.performance_mode;
  {
    std::lock_guard lock(impl_->scheduling_mutex);
    impl_->scheduling = {};
  }
  impl_->frame_cadence_enabled = !cadence_disabled;
  if (impl_->frame_cadence_enabled) {
    std::lock_guard lock(impl_->frame_cadence_mutex);
    (void)impl_->frame_cadence.Configure(options.frame_cadence);
  }

  try {
    impl_->device.copy_from(device);
    impl_->context.copy_from(context);
    impl_->sink = sink;
    impl_->monitor = monitor;
    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
      impl_->ClearRuntime();
      return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                    L"The selected monitor became unavailable before WGC "
                    L"capture started.");
    }
    const LONG monitor_width =
        monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const LONG monitor_height =
        monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    if (monitor_width <= 0 || monitor_height <= 0) {
      impl_->ClearRuntime();
      return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                    L"The selected monitor has an invalid display area.");
    }
    impl_->monitor_device_name = monitor_info.szDevice;
    impl_->expected_monitor_width = static_cast<uint32_t>(monitor_width);
    impl_->expected_monitor_height = static_cast<uint32_t>(monitor_height);
    impl_->direct3d_device = CreateDirect3DDevice(device);
    impl_->item = CreateMonitorItem(monitor);
    const auto size = impl_->item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
      impl_->ClearRuntime();
      return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                    L"WGC monitor capture item has an invalid size.");
    }
    impl_->expected_width = static_cast<uint32_t>(size.Width);
    impl_->expected_height = static_cast<uint32_t>(size.Height);
    if (options.owned_texture_pool_capacity != 0) {
      std::wstring pool_error;
      if (!impl_->texture_pool.Initialize(
              device,
              BgraTexturePoolConfig{impl_->expected_width,
                                    impl_->expected_height,
                                    options.owned_texture_pool_capacity},
              &pool_error)) {
        impl_->ClearRuntime();
        return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                      pool_error.empty()
                          ? L"Could not initialize the WGC texture pool."
                          : std::move(pool_error));
      }
    }
    impl_->item_closed_token = impl_->item.Closed(
        [implementation = impl_.get()](const GraphicsCaptureItem&, auto const&) {
          implementation->CaptureItemClosed();
        });
    impl_->item_closed_registered = true;

    impl_->frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->direct3d_device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
    impl_->frame_arrived_token = impl_->frame_pool.FrameArrived(
        [implementation = impl_.get()](
            const Direct3D11CaptureFramePool& sender, auto const&) {
          implementation->FrameArrived(sender);
        });
    impl_->frame_arrived_registered = true;
    impl_->session = impl_->frame_pool.CreateCaptureSession(impl_->item);
    if (ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled")) {
      impl_->session.IsCursorCaptureEnabled(options.capture_cursor);
    }

    impl_->running.store(true);
    impl_->session.StartCapture();
    return Result(WgcMonitorCaptureSessionStatus::Success, L"");
  } catch (const winrt::hresult_error& error) {
    impl_->running.store(false);
    (void)Stop();
    return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                  L"WGC capture start failed with HRESULT " +
                      HResultToHex(error.code()) + L": " +
                      std::wstring(error.message()));
  } catch (...) {
    impl_->running.store(false);
    (void)Stop();
    return Result(WgcMonitorCaptureSessionStatus::StartFailed,
                  L"WGC capture start failed unexpectedly.");
  }
}

WgcMonitorCaptureSessionResult WgcMonitorCaptureSession::Stop() {
  impl_->running.store(false);
  std::wstring stop_error;

  const auto remember_stop_error = [&](std::wstring message) {
    if (!stop_error.empty()) {
      stop_error += L" ";
    }
    stop_error += std::move(message);
  };

  try {
    if (impl_->item && impl_->item_closed_registered) {
      impl_->item.Closed(impl_->item_closed_token);
    }
  } catch (const winrt::hresult_error& error) {
    remember_stop_error(L"WGC capture-item callback removal failed with "
                        L"HRESULT " +
                        HResultToHex(error.code()) + L": " +
                        std::wstring(error.message()));
  } catch (...) {
    remember_stop_error(
        L"WGC capture-item callback removal failed unexpectedly.");
  }
  impl_->item_closed_registered = false;

  try {
    if (impl_->session) {
      impl_->session.Close();
    }
  } catch (const winrt::hresult_error& error) {
    remember_stop_error(L"WGC capture-session close failed with HRESULT " +
                        HResultToHex(error.code()) + L": " +
                        std::wstring(error.message()));
  } catch (...) {
    remember_stop_error(L"WGC capture-session close failed unexpectedly.");
  }

  try {
    if (impl_->frame_pool && impl_->frame_arrived_registered) {
      impl_->frame_pool.FrameArrived(impl_->frame_arrived_token);
    }
  } catch (const winrt::hresult_error& error) {
    remember_stop_error(L"WGC frame callback removal failed with HRESULT " +
                        HResultToHex(error.code()) + L": " +
                        std::wstring(error.message()));
  } catch (...) {
    remember_stop_error(L"WGC frame callback removal failed unexpectedly.");
  }
  impl_->frame_arrived_registered = false;

  try {
    if (impl_->frame_pool) {
      impl_->frame_pool.Close();
    }
  } catch (const winrt::hresult_error& error) {
    remember_stop_error(L"WGC frame-pool close failed with HRESULT " +
                        HResultToHex(error.code()) + L": " +
                        std::wstring(error.message()));
  } catch (...) {
    remember_stop_error(L"WGC frame-pool close failed unexpectedly.");
  }

  {
    std::unique_lock lock(impl_->callback_mutex);
    impl_->callback_finished.wait(lock, [this] {
      return impl_->active_callback_count.load() == 0;
    });
  }
  if (impl_->context != nullptr) {
    impl_->context->Flush();
  }
  impl_->ClearRuntime();

  if (!stop_error.empty()) {
    return Result(WgcMonitorCaptureSessionStatus::StopFailed,
                  std::move(stop_error));
  }
  return Result(WgcMonitorCaptureSessionStatus::Success, L"");
}

bool WgcMonitorCaptureSession::IsRunning() const noexcept {
  return impl_->running.load();
}

WgcMonitorCaptureSessionSnapshot WgcMonitorCaptureSession::Snapshot() const {
  impl_->PollSelectedMonitorTopology();
  impl_->PollD3D11Device();
  WgcMonitorCaptureSessionSnapshot snapshot;
  snapshot.supported = IsWgcSupported();
  snapshot.running = impl_->running.load();
  snapshot.received_frame_count = impl_->received_frame_count.load();
  snapshot.accepted_frame_count = impl_->accepted_frame_count.load();
  snapshot.rate_limited_frame_count =
      impl_->rate_limited_frame_count.load();
  snapshot.dropped_frame_count = impl_->dropped_frame_count.load();
  snapshot.texture_pool_exhausted_frame_count =
      impl_->texture_pool_exhausted_frame_count.load();
  snapshot.texture_copy_submission_count =
      impl_->texture_copy_submission_count.load();
  snapshot.last_texture_copy_submission_latency_ns =
      impl_->last_texture_copy_submission_latency_ns.load();
  snapshot.maximum_texture_copy_submission_latency_ns =
      impl_->maximum_texture_copy_submission_latency_ns.load();
  snapshot.total_texture_copy_submission_latency_ns =
      impl_->total_texture_copy_submission_latency_ns.load();
  {
    std::lock_guard lock(impl_->scheduling_mutex);
    snapshot.scheduling = impl_->scheduling;
  }
  snapshot.texture_pool = impl_->texture_pool.IsInitialized()
                              ? impl_->texture_pool.stats()
                              : impl_->last_texture_pool_stats;
  snapshot.first_timestamp_ticks = impl_->first_timestamp_ticks.load();
  snapshot.last_timestamp_ticks = impl_->last_timestamp_ticks.load();
  snapshot.fault = impl_->fault_latch.Snapshot();
  snapshot.error = snapshot.fault.message;
  return snapshot;
}

const wchar_t* WgcMonitorCaptureSessionStatusName(
    WgcMonitorCaptureSessionStatus status) noexcept {
  switch (status) {
    case WgcMonitorCaptureSessionStatus::Success:
      return L"success";
    case WgcMonitorCaptureSessionStatus::InvalidConfig:
      return L"invalid config";
    case WgcMonitorCaptureSessionStatus::Unsupported:
      return L"unsupported";
    case WgcMonitorCaptureSessionStatus::StartFailed:
      return L"start failed";
    case WgcMonitorCaptureSessionStatus::StopFailed:
      return L"stop failed";
  }
  return L"unknown";
}

const wchar_t* WgcMonitorCaptureFaultKindName(
    WgcMonitorCaptureFaultKind kind) noexcept {
  switch (kind) {
    case WgcMonitorCaptureFaultKind::None:
      return L"none";
    case WgcMonitorCaptureFaultKind::MonitorResized:
      return L"monitor resized";
    case WgcMonitorCaptureFaultKind::MonitorDisconnected:
      return L"monitor disconnected";
    case WgcMonitorCaptureFaultKind::D3D11DeviceLost:
      return L"D3D11 device lost";
    case WgcMonitorCaptureFaultKind::InvalidFrame:
      return L"invalid frame";
    case WgcMonitorCaptureFaultKind::FrameCopyFailed:
      return L"frame copy failed";
    case WgcMonitorCaptureFaultKind::SinkFailed:
      return L"sink failed";
    case WgcMonitorCaptureFaultKind::CallbackFailed:
      return L"callback failed";
  }
  return L"unknown";
}

bool IsSelectedMonitorTopologyFault(
    WgcMonitorCaptureFaultKind kind) noexcept {
  return kind == WgcMonitorCaptureFaultKind::MonitorResized ||
         kind == WgcMonitorCaptureFaultKind::MonitorDisconnected;
}

bool IsWgcD3D11DeviceFault(
    WgcMonitorCaptureFaultKind kind) noexcept {
  return kind == WgcMonitorCaptureFaultKind::D3D11DeviceLost;
}

}  // namespace olouie::capture
