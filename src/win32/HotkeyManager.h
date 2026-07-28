#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/AppCommand.h"

namespace olouie::win32 {

struct HotkeyBinding {
  int id = 0;
  UINT modifiers = 0;
  UINT virtual_key = 0;
  app::AppCommand command = app::AppCommand::OpenSettings;
  std::wstring label;
};

struct HotkeyRegistrationApi {
  std::function<bool(HWND, int, UINT, UINT)> register_hotkey;
  std::function<bool(HWND, int)> unregister_hotkey;
  std::function<DWORD()> last_error;

  static HotkeyRegistrationApi Win32();
};

class HotkeyManager final {
 public:
  using CommandSink = std::function<void(app::AppCommand)>;

  explicit HotkeyManager(
      HotkeyRegistrationApi registration_api = HotkeyRegistrationApi::Win32());
  ~HotkeyManager();

  HotkeyManager(const HotkeyManager&) = delete;
  HotkeyManager& operator=(const HotkeyManager&) = delete;

  void SetCommandSink(CommandSink sink);
  bool Register(HWND owner, const HotkeyBinding& binding, std::wstring* error);
  bool ReplaceAll(HWND owner, const std::vector<HotkeyBinding>& bindings,
                  std::wstring* error);
  void UnregisterAll();
  bool HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  std::vector<HotkeyBinding> Bindings() const;

 private:
  bool ValidateBindings(const std::vector<HotkeyBinding>& bindings,
                        std::wstring* error) const;

  HWND owner_ = nullptr;
  std::map<int, HotkeyBinding> bindings_;
  CommandSink command_sink_;
  HotkeyRegistrationApi registration_api_;
};

}  // namespace olouie::win32
