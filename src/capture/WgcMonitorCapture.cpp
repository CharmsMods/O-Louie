#include "capture/WgcMonitorCapture.h"

#include "capture/CapturedVideoFrameSink.h"

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
#include <thread>

namespace olouie::capture {
namespace {

using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Direct3DDxgiInterfaceAccess =
    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

std::wstring ResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
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
  if (device == nullptr || width == 0 || height == 0 || texture == nullptr) {
    return E_INVALIDARG;
  }

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

bool IsWgcSupported() {
  return GraphicsCaptureSession::IsSupported();
}

WgcSmokeResult RunWgcMonitorSmoke(HMONITOR monitor, ID3D11Device* device,
                                  std::chrono::milliseconds duration) {
  WgcSmokeResult result;
  result.supported = IsWgcSupported();

  if (!result.supported) {
    result.error = L"Windows Graphics Capture is not supported on this system.";
    return result;
  }

  if (monitor == nullptr) {
    result.error = L"WGC smoke needs a valid monitor handle.";
    return result;
  }

  if (device == nullptr) {
    result.error = L"WGC smoke needs a valid D3D11 device.";
    return result;
  }

  try {
    const auto d3d_device = CreateDirect3DDevice(device);
    const auto item = CreateMonitorItem(monitor);
    const auto size = item.Size();

    auto frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3d_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    std::atomic_uint32_t frame_count{0};
    std::atomic_int64_t first_timestamp{0};
    std::atomic_int64_t last_timestamp{0};

    const auto token = frame_pool.FrameArrived(
        [&](Direct3D11CaptureFramePool const& sender, auto const&) {
          for (;;) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
              break;
            }

            const int64_t timestamp = frame.SystemRelativeTime().count();
            int64_t expected = 0;
            first_timestamp.compare_exchange_strong(expected, timestamp);
            last_timestamp.store(timestamp);
            frame_count.fetch_add(1);
          }
        });

    auto session = frame_pool.CreateCaptureSession(item);
    if (ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled")) {
      session.IsCursorCaptureEnabled(true);
    }

    session.StartCapture();
    std::this_thread::sleep_for(duration);
    session.Close();
    frame_pool.FrameArrived(token);
    frame_pool.Close();

    result.frame_count = frame_count.load();
    result.first_timestamp_ticks = first_timestamp.load();
    result.last_timestamp_ticks = last_timestamp.load();
  } catch (const winrt::hresult_error& error) {
    result.error = L"WGC smoke failed with HRESULT " +
                   ResultToHex(error.code()) + L": " +
                   std::wstring(error.message());
  }

  return result;
}

WgcFrameCopySmokeResult RunWgcMonitorFrameCopySmoke(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames) {
  return RunWgcMonitorFrameCopySmoke(monitor, device, context, duration,
                                     max_copied_frames, nullptr);
}

WgcFrameCopySmokeResult RunWgcMonitorFrameCopySmoke(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames,
    ICapturedVideoFrameSink* sink) {
  WgcFrameCopySmokeResult result;
  result.supported = IsWgcSupported();

  if (!result.supported) {
    result.error = L"Windows Graphics Capture is not supported on this system.";
    return result;
  }
  if (monitor == nullptr) {
    result.error = L"WGC frame-copy smoke needs a valid monitor handle.";
    return result;
  }
  if (device == nullptr || context == nullptr) {
    result.error = L"WGC frame-copy smoke needs a D3D11 device and context.";
    return result;
  }
  if (duration.count() <= 0 || max_copied_frames == 0) {
    result.error = L"WGC frame-copy smoke needs a positive duration and frame cap.";
    return result;
  }

  try {
    const auto d3d_device = CreateDirect3DDevice(device);
    const auto item = CreateMonitorItem(monitor);
    const auto size = item.Size();

    auto frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3d_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    std::atomic_uint32_t frame_count{0};
    std::atomic_uint32_t accepted_frame_count{0};
    std::atomic_uint32_t dropped_frame_count{0};
    std::atomic_int64_t first_timestamp{0};
    std::atomic_int64_t last_timestamp{0};
    std::atomic_bool callback_failed{false};
    VideoFrameQueue frame_queue(
        VideoFrameQueueOptions{max_copied_frames,
                               VideoFrameOverflowPolicy::DropNewest});
    std::mutex wait_mutex;
    std::mutex error_mutex;
    std::condition_variable frames_ready;
    std::wstring callback_error;

    const auto set_callback_error = [&](std::wstring error) {
      if (!callback_failed.exchange(true)) {
        std::lock_guard lock(error_mutex);
        callback_error = std::move(error);
      }
      frames_ready.notify_one();
    };

    const auto token = frame_pool.FrameArrived(
        [&](Direct3D11CaptureFramePool const& sender, auto const&) {
          for (;;) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
              break;
            }

            const int64_t timestamp = frame.SystemRelativeTime().count();
            int64_t expected = 0;
            first_timestamp.compare_exchange_strong(expected, timestamp);
            last_timestamp.store(timestamp);
            frame_count.fetch_add(1);

            if (callback_failed.load()) {
              frames_ready.notify_one();
              continue;
            }

            try {
              const auto content_size = frame.ContentSize();
              if (content_size.Width <= 0 || content_size.Height <= 0) {
                set_callback_error(
                    L"WGC frame-copy smoke received an invalid frame size.");
                continue;
              }

              auto source_texture = GetTextureFromSurface(frame.Surface());
              D3D11_TEXTURE2D_DESC source_desc{};
              source_texture->GetDesc(&source_desc);
              if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                set_callback_error(
                    L"WGC frame-copy smoke received a non-BGRA frame.");
                continue;
              }

              const auto width = static_cast<uint32_t>(content_size.Width);
              const auto height = static_cast<uint32_t>(content_size.Height);
              winrt::com_ptr<ID3D11Texture2D> copied_texture;
              const HRESULT create_result = CreateCopiedBgraTexture(
                  device, width, height, copied_texture.put());
              if (FAILED(create_result)) {
                set_callback_error(
                    L"Could not create app-owned WGC BGRA texture (" +
                    ResultToHex(create_result) + L").");
                continue;
              }

              D3D11_BOX source_box{};
              source_box.left = 0;
              source_box.top = 0;
              source_box.front = 0;
              source_box.right = width;
              source_box.bottom = height;
              source_box.back = 1;
              context->CopySubresourceRegion(copied_texture.get(), 0, 0, 0, 0,
                                             source_texture.get(), 0,
                                             &source_box);

              OwnedVideoFrame copied_frame;
              copied_frame.texture = std::move(copied_texture);
              copied_frame.width = width;
              copied_frame.height = height;
              copied_frame.timestamp_ticks = timestamp;

              if (sink != nullptr) {
                const auto delivered =
                    DispatchCapturedVideoFrame(sink, std::move(copied_frame));
                if (!delivered.Succeeded()) {
                  std::wstring message = delivered.message;
                  if (message.empty()) {
                    message = L"Captured video frame sink failed with status " +
                              std::wstring(
                                  CapturedVideoFrameSinkStatusName(
                                      delivered.status)) +
                              L".";
                  }
                  set_callback_error(std::move(message));
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
              } else {
                const auto push = frame_queue.Push(std::move(copied_frame));
                if (push.Queued()) {
                  accepted_frame_count.fetch_add(1);
                }
                if (push.Dropped()) {
                  dropped_frame_count.fetch_add(push.dropped_frame_count);
                }
              }
              frames_ready.notify_one();
            } catch (const winrt::hresult_error& error) {
              set_callback_error(L"WGC frame-copy callback failed with HRESULT " +
                                 ResultToHex(error.code()) + L": " +
                                 std::wstring(error.message()));
            } catch (...) {
              set_callback_error(L"WGC frame-copy callback failed unexpectedly.");
            }
          }
        });

    auto session = frame_pool.CreateCaptureSession(item);
    if (ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled")) {
      session.IsCursorCaptureEnabled(true);
    }

    session.StartCapture();
    {
      std::unique_lock lock(wait_mutex);
      frames_ready.wait_for(lock, duration, [&] {
        return (sink != nullptr
                    ? accepted_frame_count.load() +
                          dropped_frame_count.load() >=
                          max_copied_frames
                    : frame_queue.Size() >= max_copied_frames) ||
               callback_failed.load();
      });
    }
    session.Close();
    frame_pool.FrameArrived(token);
    frame_pool.Close();
    context->Flush();

    result.frame_count = frame_count.load();
    result.copied_frame_count = 0;
    result.first_timestamp_ticks = first_timestamp.load();
    result.last_timestamp_ticks = last_timestamp.load();
    {
      std::lock_guard lock(error_mutex);
      result.error = callback_error;
    }
    const auto queue_stats = frame_queue.stats();
    if (sink != nullptr) {
      result.copied_frame_count = accepted_frame_count.load();
      result.dropped_frame_count = dropped_frame_count.load();
    } else {
      result.dropped_frame_count = static_cast<uint32_t>(
          queue_stats.dropped_newest_count + queue_stats.dropped_oldest_count +
          queue_stats.dropped_backlog_count);
      result.frames = frame_queue.Drain();
      result.copied_frame_count = static_cast<uint32_t>(result.frames.size());
    }
  } catch (const winrt::hresult_error& error) {
    result.error = L"WGC frame-copy smoke failed with HRESULT " +
                   ResultToHex(error.code()) + L": " +
                   std::wstring(error.message());
  }

  return result;
}

}  // namespace olouie::capture
