#include "win32/HiddenWindow.h"

namespace olouie::win32 {

HiddenWindow::~HiddenWindow() {
  Destroy();
}

bool HiddenWindow::Create(HINSTANCE instance, std::wstring_view class_name,
                          MessageHandler handler) {
  if (hwnd_ != nullptr) {
    return true;
  }

  instance_ = instance;
  handler_ = std::move(handler);
  class_name_ = std::wstring(class_name) + L".HiddenWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &HiddenWindow::WndProc;
  window_class.hInstance = instance_;
  window_class.lpszClassName = class_name_.c_str();

  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  hwnd_ = CreateWindowExW(0, class_name_.c_str(), L"O'Louie Hidden Window", 0,
                          0, 0, 0, 0, nullptr, nullptr, instance_, this);

  return hwnd_ != nullptr;
}

void HiddenWindow::Destroy() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  if (instance_ != nullptr && !class_name_.empty()) {
    UnregisterClassW(class_name_.c_str(), instance_);
    class_name_.clear();
    instance_ = nullptr;
  }
}

HWND HiddenWindow::hwnd() const noexcept {
  return hwnd_;
}

LRESULT CALLBACK HiddenWindow::WndProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  HiddenWindow* self =
      reinterpret_cast<HiddenWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<HiddenWindow*>(create->lpCreateParams);
    self->hwnd_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  }

  LRESULT result = 0;
  if (self != nullptr && self->handler_) {
    result = self->handler_(window, message, wparam, lparam);
  } else {
    result = DefWindowProcW(window, message, wparam, lparam);
  }

  if (message == WM_NCDESTROY && self != nullptr) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    self->hwnd_ = nullptr;
  }

  return result;
}

}  // namespace olouie::win32
