#include "ui/ImGuiDx11Host.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

#include "win32/WindowAffinity.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace olouie::ui {
namespace {

constexpr UINT_PTR kRenderTimerId = 1;
constexpr UINT kRenderIntervalMs = 33;
constexpr wchar_t kSettingsWindowClass[] = L"O'Louie.ImGuiDx11Settings";

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

void ConfigureStyle(float dpi_scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.ChildRounding = 4.0f;
  style.FrameRounding = 3.0f;
  style.PopupRounding = 4.0f;
  style.ScrollbarRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.TabRounding = 3.0f;
  style.WindowPadding = ImVec2(18.0f, 16.0f);
  style.FramePadding = ImVec2(9.0f, 6.0f);
  style.ItemSpacing = ImVec2(10.0f, 9.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.ScaleAllSizes(std::max(1.0f, dpi_scale));
  style.FontScaleDpi = std::max(1.0f, dpi_scale);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.090f, 0.105f, 1.0f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.095f, 0.112f, 0.128f, 1.0f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.090f, 0.105f, 0.120f, 0.98f);
  colors[ImGuiCol_Border] = ImVec4(0.225f, 0.255f, 0.275f, 0.75f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.125f, 0.145f, 0.160f, 1.0f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.165f, 0.205f, 0.215f, 1.0f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.180f, 0.260f, 0.265f, 1.0f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.060f, 0.075f, 0.085f, 1.0f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.100f, 0.110f, 1.0f);
  colors[ImGuiCol_Button] = ImVec4(0.105f, 0.390f, 0.405f, 1.0f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.125f, 0.485f, 0.495f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.095f, 0.320f, 0.335f, 1.0f);
  colors[ImGuiCol_Header] = ImVec4(0.100f, 0.305f, 0.320f, 0.85f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.125f, 0.405f, 0.420f, 0.95f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.105f, 0.335f, 0.350f, 1.0f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.375f, 0.865f, 0.735f, 1.0f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.310f, 0.720f, 0.660f, 1.0f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.420f, 0.895f, 0.745f, 1.0f);
  colors[ImGuiCol_Tab] = ImVec4(0.095f, 0.120f, 0.135f, 1.0f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.130f, 0.360f, 0.370f, 1.0f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.105f, 0.305f, 0.320f, 1.0f);
  colors[ImGuiCol_Text] = ImVec4(0.900f, 0.925f, 0.935f, 1.0f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.520f, 0.565f, 0.590f, 1.0f);
}

}  // namespace

void EventDrivenRenderState::SetVisible(bool visible) noexcept {
  stats_.visible = visible;
}

bool EventDrivenRenderState::TryBeginFrame() noexcept {
  ++stats_.render_request_count;
  if (!stats_.visible) {
    ++stats_.hidden_request_count;
    return false;
  }
  ++stats_.rendered_frame_count;
  return true;
}

const EventDrivenRenderStats& EventDrivenRenderState::stats() const noexcept {
  return stats_;
}

ImGuiDx11Host::~ImGuiDx11Host() {
  Destroy();
}

bool ImGuiDx11Host::Create(HINSTANCE instance, std::wstring_view title,
                           FrameSink frame_sink, CloseSink close_sink,
                           DiagnosticSink diagnostic_sink,
                           std::wstring* error) {
  if (created()) {
    return true;
  }
  if (instance == nullptr || !frame_sink) {
    SetError(error, L"Settings renderer initialization is incomplete.");
    return false;
  }

  instance_ = instance;
  frame_sink_ = std::move(frame_sink);
  close_sink_ = std::move(close_sink);
  diagnostic_sink_ = std::move(diagnostic_sink);
  class_name_ = kSettingsWindowClass;

  ImGui_ImplWin32_EnableDpiAwareness();
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = &ImGuiDx11Host::WndProc;
  window_class.hInstance = instance_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = class_name_.c_str();
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    SetError(error, L"Could not register the settings window class.");
    Destroy();
    return false;
  }

  const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
      MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  const int width = static_cast<int>(840.0f * std::max(1.0f, scale));
  const int height = static_cast<int>(650.0f * std::max(1.0f, scale));
  const int screen_width = GetSystemMetrics(SM_CXSCREEN);
  const int screen_height = GetSystemMetrics(SM_CYSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, class_name_.c_str(), std::wstring(title).c_str(),
      WS_OVERLAPPEDWINDOW, std::max(0, (screen_width - width) / 2),
      std::max(0, (screen_height - height) / 2), width, height, nullptr,
      nullptr, instance_, this);
  if (hwnd_ == nullptr) {
    SetError(error, L"Could not create the settings window.");
    Destroy();
    return false;
  }

  std::wstring affinity_error;
  capture_excluded_ =
      win32::TryExcludeFromCapture(hwnd_, &affinity_error);
  if (!capture_excluded_) {
    Report(affinity_error);
  }

  if (!CreateDevice(error)) {
    Destroy();
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ConfigureStyle(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_));
  const bool win32_initialized = ImGui_ImplWin32_Init(hwnd_);
  const bool dx11_initialized =
      win32_initialized && ImGui_ImplDX11_Init(device_.get(), context_.get());
  if (!win32_initialized || !dx11_initialized) {
    if (dx11_initialized) {
      ImGui_ImplDX11_Shutdown();
    }
    if (win32_initialized) {
      ImGui_ImplWin32_Shutdown();
    }
    ImGui::DestroyContext();
    SetError(error, L"Dear ImGui Win32/D3D11 backend initialization failed.");
    Destroy();
    return false;
  }
  imgui_initialized_ = true;
  return true;
}

bool ImGuiDx11Host::Show(std::wstring* error) {
  if (!created() || !imgui_initialized_) {
    SetError(error, L"Settings renderer is not initialized.");
    return false;
  }
  if (visible()) {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    return true;
  }
  if (SetTimer(hwnd_, kRenderTimerId, kRenderIntervalMs, nullptr) == 0) {
    SetError(error, L"Could not start the capped settings render timer.");
    return false;
  }
  render_state_.SetVisible(true);
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetForegroundWindow(hwnd_);
  RenderFrame();
  return true;
}

void ImGuiDx11Host::Hide() {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kRenderTimerId);
    ShowWindow(hwnd_, SW_HIDE);
  }
  render_state_.SetVisible(false);
}

void ImGuiDx11Host::Destroy() {
  Hide();
  if (imgui_initialized_) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imgui_initialized_ = false;
  }
  DestroyDevice();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (instance_ != nullptr && !class_name_.empty()) {
    UnregisterClassW(class_name_.c_str(), instance_);
  }
  instance_ = nullptr;
  class_name_.clear();
  frame_sink_ = {};
  close_sink_ = {};
  diagnostic_sink_ = {};
}

bool ImGuiDx11Host::created() const noexcept {
  return hwnd_ != nullptr;
}

bool ImGuiDx11Host::visible() const noexcept {
  return render_state_.stats().visible;
}

HWND ImGuiDx11Host::hwnd() const noexcept {
  return hwnd_;
}

ImGuiDx11HostStats ImGuiDx11Host::stats() const noexcept {
  ImGuiDx11HostStats result;
  result.render = render_state_.stats();
  result.timer_tick_count = timer_tick_count_;
  result.capture_excluded = capture_excluded_;
  return result;
}

LRESULT CALLBACK ImGuiDx11Host::WndProc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<ImGuiDx11Host*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ImGuiDx11Host*>(create->lpCreateParams);
    self->hwnd_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  }
  if (self != nullptr) {
    return self->HandleMessage(window, message, wparam, lparam);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ImGuiDx11Host::HandleMessage(HWND window, UINT message,
                                     WPARAM wparam, LPARAM lparam) {
  if (imgui_initialized_ &&
      ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
    return TRUE;
  }

  switch (message) {
    case WM_TIMER:
      if (wparam == kRenderTimerId) {
        ++timer_tick_count_;
        if (!IsIconic(window)) {
          RenderFrame();
        }
        return 0;
      }
      break;
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED && swap_chain_ != nullptr) {
        DestroyRenderTarget();
        const HRESULT resize = swap_chain_->ResizeBuffers(
            0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(resize)) {
          std::wstring ignored;
          CreateRenderTarget(&ignored);
        } else {
          Report(L"Settings swap-chain resize failed.");
        }
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CLOSE:
      Hide();
      if (close_sink_) {
        close_sink_();
      }
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool ImGuiDx11Host::CreateDevice(std::wstring* error) {
  DXGI_SWAP_CHAIN_DESC description{};
  description.BufferCount = 2;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.BufferDesc.RefreshRate.Numerator = 60;
  description.BufferDesc.RefreshRate.Denominator = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = hwnd_;
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  constexpr D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected{};
  HRESULT result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, feature_levels,
      static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
      &description, swap_chain_.put(), device_.put(), &selected,
      context_.put());
  if (result == DXGI_ERROR_UNSUPPORTED) {
    result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
        &description, swap_chain_.put(), device_.put(), &selected,
        context_.put());
  }
  if (FAILED(result)) {
    SetError(error, L"Could not create the settings D3D11 device/swap chain.");
    return false;
  }
  return CreateRenderTarget(error);
}

bool ImGuiDx11Host::CreateRenderTarget(std::wstring* error) {
  winrt::com_ptr<ID3D11Texture2D> back_buffer;
  if (swap_chain_ == nullptr || device_ == nullptr ||
      FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()))) ||
      FAILED(device_->CreateRenderTargetView(back_buffer.get(), nullptr,
                                             render_target_.put()))) {
    SetError(error, L"Could not create the settings render target.");
    return false;
  }
  return true;
}

void ImGuiDx11Host::DestroyRenderTarget() {
  render_target_ = nullptr;
}

void ImGuiDx11Host::DestroyDevice() {
  DestroyRenderTarget();
  swap_chain_ = nullptr;
  context_ = nullptr;
  device_ = nullptr;
}

void ImGuiDx11Host::RenderFrame() {
  if (!render_state_.TryBeginFrame() || !imgui_initialized_ ||
      render_target_ == nullptr || !frame_sink_) {
    return;
  }

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  frame_sink_();
  ImGui::Render();

  constexpr float clear_color[4] = {0.075f, 0.090f, 0.105f, 1.0f};
  ID3D11RenderTargetView* target = render_target_.get();
  context_->OMSetRenderTargets(1, &target, nullptr);
  context_->ClearRenderTargetView(target, clear_color);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  const HRESULT present = swap_chain_->Present(1, 0);
  if (FAILED(present) && present != DXGI_STATUS_OCCLUDED) {
    Report(L"Settings swap-chain presentation failed.");
    Hide();
  }
}

void ImGuiDx11Host::Report(std::wstring_view message) const {
  if (diagnostic_sink_) {
    diagnostic_sink_(message);
  }
}

}  // namespace olouie::ui
