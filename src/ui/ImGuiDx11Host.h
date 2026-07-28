#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace olouie::ui {

struct EventDrivenRenderStats {
  bool visible = false;
  uint64_t render_request_count = 0;
  uint64_t rendered_frame_count = 0;
  uint64_t hidden_request_count = 0;
};

class EventDrivenRenderState final {
 public:
  void SetVisible(bool visible) noexcept;
  bool TryBeginFrame() noexcept;
  const EventDrivenRenderStats& stats() const noexcept;

 private:
  EventDrivenRenderStats stats_;
};

struct ImGuiDx11HostStats {
  EventDrivenRenderStats render;
  uint64_t timer_tick_count = 0;
  bool capture_excluded = false;
};

class ImGuiDx11Host final {
 public:
  using FrameSink = std::function<void()>;
  using CloseSink = std::function<void()>;
  using DiagnosticSink = std::function<void(std::wstring_view)>;

  ImGuiDx11Host() = default;
  ~ImGuiDx11Host();

  ImGuiDx11Host(const ImGuiDx11Host&) = delete;
  ImGuiDx11Host& operator=(const ImGuiDx11Host&) = delete;

  bool Create(HINSTANCE instance, std::wstring_view title,
              FrameSink frame_sink, CloseSink close_sink,
              DiagnosticSink diagnostic_sink, std::wstring* error);
  bool Show(std::wstring* error);
  void Hide();
  void Destroy();

  bool created() const noexcept;
  bool visible() const noexcept;
  HWND hwnd() const noexcept;
  ImGuiDx11HostStats stats() const noexcept;

 private:
  static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam);
  LRESULT HandleMessage(HWND window, UINT message, WPARAM wparam,
                        LPARAM lparam);
  bool CreateDevice(std::wstring* error);
  bool CreateRenderTarget(std::wstring* error);
  void DestroyRenderTarget();
  void DestroyDevice();
  void RenderFrame();
  void Report(std::wstring_view message) const;

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  std::wstring class_name_;
  FrameSink frame_sink_;
  CloseSink close_sink_;
  DiagnosticSink diagnostic_sink_;
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<IDXGISwapChain> swap_chain_;
  winrt::com_ptr<ID3D11RenderTargetView> render_target_;
  EventDrivenRenderState render_state_;
  uint64_t timer_tick_count_ = 0;
  bool imgui_initialized_ = false;
  bool capture_excluded_ = false;
};

}  // namespace olouie::ui
