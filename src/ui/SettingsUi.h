#pragma once

#include <windows.h>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/AudioEndpointManager.h"
#include "audio/MicMonitorSession.h"
#include "diagnostics/DiagnosticsSnapshot.h"
#include "graphics/DisplayManager.h"
#include "settings/Settings.h"
#include "ui/ImGuiDx11Host.h"

namespace olouie::ui {

enum class SettingsUiInitialTab {
  General,
  Audio,
  Diagnostics,
};

class SettingsUi final {
 public:
  using ApplySink =
      std::function<bool(const settings::AppSettings&, std::wstring*)>;
  using DiagnosticSink = std::function<void(std::wstring_view)>;
  using MicMonitorStartSink = std::function<audio::MicMonitorCommandResult(
      const audio::MicMonitorOptions&)>;
  using MicMonitorStopSink =
      std::function<audio::MicMonitorCommandResult()>;
  using MicMonitorSnapshotSource =
      std::function<audio::MicMonitorSnapshot()>;

  SettingsUi() = default;
  ~SettingsUi();

  SettingsUi(const SettingsUi&) = delete;
  SettingsUi& operator=(const SettingsUi&) = delete;

  void Configure(HINSTANCE instance, ApplySink apply_sink,
                 DiagnosticSink diagnostic_sink,
                 MicMonitorStartSink mic_monitor_start_sink,
                 MicMonitorStopSink mic_monitor_stop_sink,
                 MicMonitorSnapshotSource mic_monitor_snapshot_source);
  bool Open(const settings::AppSettings& current,
            const settings::AppSettings& defaults, std::wstring* error,
            SettingsUiInitialTab initial_tab = SettingsUiInitialTab::General);
  void SetDiagnosticsSnapshot(
      diagnostics::DiagnosticsSnapshot snapshot);
  void Close();
  void Shutdown();

  bool visible() const noexcept;
  HWND hwnd() const noexcept;
  ImGuiDx11HostStats host_stats() const noexcept;

 private:
  void Render();
  void RenderGeneralTab();
  void RenderVideoTab();
  void RenderAudioTab();
  void RenderClipsTab();
  void RenderHotkeysTab();
  void RenderDiagnosticsTab();
  void SyncBuffersFromDraft();
  bool SyncDraftFromBuffers(std::wstring* error);
  bool SaveDraft();
  void PickOutputDirectory();
  void RefreshAudioOutputs();
  void StopMicMonitor();
  void SetStatus(std::wstring message, bool error);

  HINSTANCE instance_ = nullptr;
  ApplySink apply_sink_;
  DiagnosticSink diagnostic_sink_;
  MicMonitorStartSink mic_monitor_start_sink_;
  MicMonitorStopSink mic_monitor_stop_sink_;
  MicMonitorSnapshotSource mic_monitor_snapshot_source_;
  ImGuiDx11Host host_;
  settings::AppSettings draft_;
  settings::AppSettings defaults_;
  diagnostics::DiagnosticsSnapshot diagnostics_;
  std::vector<graphics::MonitorInfo> monitors_;
  std::vector<audio::AudioEndpointInfo> audio_outputs_;
  std::wstring audio_output_enumeration_error_;
  std::array<char, 1024> output_directory_{};
  std::array<char, 96> toggle_recording_{};
  std::array<char, 96> save_first_preset_{};
  std::array<char, 96> save_third_preset_{};
  std::array<char, 96> bookmark_{};
  std::string status_text_;
  bool status_is_error_ = false;
  bool select_audio_tab_ = false;
  bool select_diagnostics_tab_ = false;
};

}  // namespace olouie::ui
