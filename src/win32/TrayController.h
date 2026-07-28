#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <cstddef>
#include <string_view>

#include "app/AppCommand.h"

namespace olouie::win32 {

enum class TrayRecordingState {
  Idle,
  Starting,
  Recording,
  Stopping,
};

class TrayController final {
 public:
  using CommandSink = std::function<void(app::AppCommand)>;

  TrayController() = default;
  ~TrayController();

  TrayController(const TrayController&) = delete;
  TrayController& operator=(const TrayController&) = delete;

  bool Create(HWND owner, std::wstring_view tooltip);
  void Destroy();

  void SetCommandSink(CommandSink sink);
  bool HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  bool HandleCommand(WPARAM command_id);
  void ShowInfo(std::wstring_view title, std::wstring_view message);
  void SetRecordingState(TrayRecordingState state) noexcept;
  void SetNotificationsEnabled(bool enabled) noexcept;
  void SetClipPresetDurations(int first_seconds, int second_seconds,
                              int third_seconds, int custom_seconds) noexcept;
  void SetRecoveryState(size_t exportable_count, size_t discardable_count,
                        bool busy) noexcept;

 private:
  void ShowContextMenu();
  void Emit(app::AppCommand command);

  HWND owner_ = nullptr;
  NOTIFYICONDATAW notify_icon_{};
  bool created_ = false;
  bool notifications_enabled_ = true;
  int first_clip_preset_seconds_ = 30;
  int second_clip_preset_seconds_ = 60;
  int third_clip_preset_seconds_ = 300;
  int custom_clip_seconds_ = 120;
  size_t recovery_exportable_count_ = 0;
  size_t recovery_discardable_count_ = 0;
  bool recovery_busy_ = false;
  TrayRecordingState recording_state_ = TrayRecordingState::Idle;
  CommandSink command_sink_;
};

}  // namespace olouie::win32
