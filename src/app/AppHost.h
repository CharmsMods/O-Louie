#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/AppCommand.h"
#include "app/SingleInstance.h"
#include "audio/MicMonitorSession.h"
#include "logging/Logger.h"
#include "logging/RuntimePaths.h"
#include "record/RecordingRecovery.h"
#include "record/VideoRecorderSession.h"
#include "settings/Settings.h"
#include "ui/SettingsUi.h"
#include "win32/HiddenWindow.h"
#include "win32/HotkeyManager.h"
#include "win32/TrayController.h"

namespace olouie::app {

class AppHost final {
 public:
  explicit AppHost(HINSTANCE instance);
  ~AppHost();

  AppHost(const AppHost&) = delete;
  AppHost& operator=(const AppHost&) = delete;

  int Run(int show_command);

 private:
  bool Initialize(int show_command);
  void Shutdown();
  void HandleCommand(AppCommand command);
  void HandleToggleRecording();
  void HandleSaveLastClip(int preset_seconds);
  void HandleAddBookmark();
  void HandleRecoverRecording();
  void HandleDiscardRecovery();
  void HandleRecorderStateChanged();
  void PostRecorderStateChanged();
  void HandleMicMonitorStateChanged();
  void PostMicMonitorStateChanged();
  void HandleRecoveryStateChanged();
  void PostRecoveryStateChanged();
  bool ApplySettings(const settings::AppSettings& candidate,
                     std::wstring* error);
  bool ConfigureHotkeys(const settings::AppSettings& settings,
                        std::wstring* error);
  void CreateVideoRecorder();
  void CreateRecordingRecovery();
  void ReconfigureRecorderIfInactive();
  void RefreshDiagnosticsSnapshot();
  audio::MicMonitorCommandResult StartMicMonitor(
      const audio::MicMonitorOptions& options);
  LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wparam,
                              LPARAM lparam);

  HINSTANCE instance_;
  logging::RuntimePaths paths_;
  logging::Logger logger_;
  settings::AppSettings default_settings_;
  settings::AppSettings settings_;
  SingleInstance single_instance_;
  SingleInstance legacy_single_instance_;
  win32::HiddenWindow hidden_window_;
  win32::TrayController tray_;
  win32::HotkeyManager hotkeys_;
  ui::SettingsUi settings_ui_;
  audio::MicMonitorSession mic_monitor_;
  std::unique_ptr<record::VideoRecorderSession> video_recorder_;
  std::unique_ptr<record::RecordingRecoverySession> recording_recovery_;
  std::filesystem::path settings_file_path_;
  std::filesystem::path executable_path_;
  uint64_t last_recorder_notification_generation_ = 0;
  record::VideoRecorderState last_recorder_notification_state_ =
      record::VideoRecorderState::Idle;
  uint64_t last_clip_notification_generation_ = 0;
  uint64_t last_recorder_diagnostics_generation_ = 0;
  uint64_t last_logged_diagnostics_generation_ = 0;
  uint64_t last_recovery_notification_generation_ = 0;
  record::RecordingRecoveryState last_recovery_notification_state_ =
      record::RecordingRecoveryState::Idle;
  uint64_t last_recovery_action_generation_ = 0;
  uint64_t last_mic_monitor_generation_ = 0;
  audio::MicMonitorState last_mic_monitor_state_ =
      audio::MicMonitorState::Idle;
  bool recording_start_pending_after_mic_monitor_ = false;
  bool recorder_reconfigure_pending_ = false;
  bool com_initialized_ = false;
  bool shutdown_complete_ = false;
};

}  // namespace olouie::app
