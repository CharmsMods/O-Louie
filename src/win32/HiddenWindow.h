#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace olouie::win32 {

class HiddenWindow final {
 public:
  using MessageHandler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;

  HiddenWindow() = default;
  ~HiddenWindow();

  HiddenWindow(const HiddenWindow&) = delete;
  HiddenWindow& operator=(const HiddenWindow&) = delete;

  bool Create(HINSTANCE instance, std::wstring_view class_name,
              MessageHandler handler);
  void Destroy();

  HWND hwnd() const noexcept;

 private:
  static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  std::wstring class_name_;
  MessageHandler handler_;
};

}  // namespace olouie::win32
