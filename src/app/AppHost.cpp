#include "app/AppHost.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "logging/RuntimePathMigration.h"
#include "product/ProductIdentity.h"
#include "settings/HotkeyBinding.h"
#include "settings/SettingsStore.h"
#include "win32/StartupRegistration.h"

namespace olouie::app {
namespace {

constexpr UINT kRecorderStateChangedMessage = WM_APP + 2;
constexpr UINT kRecoveryStateChangedMessage = WM_APP + 3;
constexpr UINT kMicMonitorStateChangedMessage = WM_APP + 4;
constexpr int kToggleRecordingHotkeyId = 1;
constexpr int kSaveLast30SecondsHotkeyId = 2;
constexpr int kSaveLast5MinutesHotkeyId = 3;
constexpr int kBookmarkHotkeyId = 4;

bool ExerciseDiagnosticsLoggingEnabled() {
  static const bool enabled = [] {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        L"OLOUIE_DIAGNOSTICS_LOGGING", value,
        static_cast<DWORD>(std::size(value)));
    return length == 1 && value[0] == L'1';
  }();
  return enabled;
}

record::VideoRecorderPipelineOptions BuildVideoRecorderOptions(
    const logging::RuntimePaths& paths,
    const settings::AppSettings& settings) {
  record::VideoRecorderPipelineOptions options;
  options.session_root_directory = paths.sessions;
  options.output_directory = settings.output_directory;
  options.monitor_device_name = settings.video.monitor_device_name;
  options.use_source_output_dimensions =
      settings.video.resolution_mode == settings::ResolutionMode::Source;
  options.capture_cursor = settings.video.capture_cursor;
  options.performance_mode = settings.video.performance_mode;
  options.drain_interval = std::chrono::milliseconds(10);

  auto& preflight = options.preflight;
  preflight.video_track_id = 1;
  preflight.encoder_config.width =
      static_cast<uint32_t>(settings.video.custom_width);
  preflight.encoder_config.height =
      static_cast<uint32_t>(settings.video.custom_height);
  preflight.encoder_config.fps_numerator =
      static_cast<uint32_t>(settings.video.fps);
  preflight.encoder_config.fps_denominator = 1;
  const uint64_t bitrate_bps =
      static_cast<uint64_t>(settings.video.bitrate_mbps) * 1000000ULL;
  preflight.encoder_config.bitrate_bps = static_cast<uint32_t>(
      std::min<uint64_t>(bitrate_bps,
                         std::numeric_limits<uint32_t>::max()));
  preflight.encoder_config.gop_seconds = settings.video.gop_seconds;
  preflight.encoder_config.max_b_frames = 0;
  preflight.encoder_config.performance_mode =
      settings.video.performance_mode;
  preflight.queue_capacity = 8;
  preflight.overflow_policy =
      capture::VideoFrameOverflowPolicy::KeepNewest;
  preflight.drain_frame_budget = 4;
  preflight.session_drain_timeout_ms = 3000;
  preflight.live.duration = std::chrono::milliseconds(0);
  preflight.live.drain_interval = options.drain_interval;
  preflight.live.max_copied_frames = 0;
  preflight.live.max_frames_per_drain_tick = 4;
  preflight.live.timestamp_frequency = 10000000;
  preflight.live.start_timebase_on_first_frame = true;

  if (settings.audio.system_mix || settings.audio.mic) {
    auto& audio_options = options.audio.emplace();
    audio_options.preflight.first_track_id = 2;
    audio_options.preflight.system_loopback = settings.audio.system_mix;
    audio_options.preflight.microphone = settings.audio.mic;
    audio_options.preflight.require_system_loopback = settings.audio.system_mix;
    audio_options.preflight.require_microphone = settings.audio.mic;
    audio_options.preflight.separate_source_tracks = true;
    audio_options.preflight.default_mixed_track = false;
    audio_options.preflight.output_sample_rate =
        static_cast<uint32_t>(settings.audio.sample_rate);
    audio_options.setup.queue_capacity = 1024;
    audio_options.setup.overflow_policy =
        audio::PreparedPcmOverflowPolicy::RejectNewest;
    audio_options.setup.aac_bitrate_bps = 192000;
    audio_options.live.duration = std::chrono::milliseconds(0);
    audio_options.live.drain_interval = options.drain_interval;
    audio_options.live.max_blocks_per_drain_tick = 64;
    audio_options.live.maintain_track_continuity = true;
    audio_options.live.performance_mode = settings.video.performance_mode;
  }
  return options;
}

bool BuildHotkeyBindings(const settings::AppSettings& settings,
                         std::vector<win32::HotkeyBinding>* bindings,
                         std::wstring* error) {
  if (bindings == nullptr) {
    if (error != nullptr) {
      *error = L"Hotkey binding output storage is null.";
    }
    return false;
  }

  struct RequestedBinding {
    int id;
    app::AppCommand command;
    const std::wstring* label;
  };
  const RequestedBinding requested[] = {
      {kToggleRecordingHotkeyId, AppCommand::ToggleRecording,
       &settings.hotkeys.toggle_recording},
      {kSaveLast30SecondsHotkeyId, AppCommand::SaveLast30Seconds,
       &settings.hotkeys.save_last_30s},
      {kSaveLast5MinutesHotkeyId, AppCommand::SaveLast5Minutes,
       &settings.hotkeys.save_last_5m},
      {kBookmarkHotkeyId, AppCommand::AddBookmark,
       &settings.hotkeys.bookmark},
  };

  std::vector<win32::HotkeyBinding> parsed_bindings;
  parsed_bindings.reserve(std::size(requested));
  for (const auto& binding : requested) {
    const auto parsed = settings::ParseHotkey(*binding.label);
    if (!parsed.Succeeded()) {
      if (error != nullptr) {
        *error = L"Invalid hotkey '" + *binding.label + L"': " +
                 parsed.message;
      }
      return false;
    }
    parsed_bindings.push_back({binding.id, parsed.hotkey.modifiers,
                               parsed.hotkey.virtual_key, binding.command,
                               parsed.hotkey.canonical_label});
  }
  *bindings = std::move(parsed_bindings);
  return true;
}

void ShowStartupFailure(std::wstring_view message) {
  MessageBoxW(nullptr, std::wstring(message).c_str(), L"O'Louie startup failed",
              MB_OK | MB_ICONERROR);
}

void ShowStartupFailure(std::string_view message) {
  MessageBoxA(nullptr, std::string(message).c_str(), "O'Louie startup failed",
              MB_OK | MB_ICONERROR);
}

}  // namespace

AppHost::AppHost(HINSTANCE instance) : instance_(instance) {}

AppHost::~AppHost() {
  Shutdown();
}

int AppHost::Run(int show_command) {
  try {
    if (!Initialize(show_command)) {
      Shutdown();
      return 1;
    }
  } catch (const std::exception& error) {
    ShowStartupFailure(error.what());
    Shutdown();
    return 1;
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  Shutdown();
  return static_cast<int>(message.wParam);
}

bool AppHost::Initialize(int show_command) {
  (void)show_command;

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    ShowStartupFailure(L"COM initialization failed.");
    return false;
  }
  com_initialized_ = true;

  const auto path_migration =
      logging::ResolveRuntimePathsWithLegacyMigration(
          product::kRuntimeFolderName, product::kLegacyRuntimeFolderName);
  paths_ = path_migration.paths;
  paths_.EnsureCreated();

  if (!logger_.Open(paths_.logs / product::kLogFileName)) {
    ShowStartupFailure(L"Could not open the O'Louie log file.");
    return false;
  }

  if (!path_migration.message.empty()) {
    if (path_migration.NeedsWarning()) {
      logger_.Warning(path_migration.message);
    } else {
      logger_.Info(path_migration.message);
    }
  }
  logger_.Info(L"Starting O'Louie foundation shell.");

  if (!single_instance_.Acquire(product::kSingleInstanceName) ||
      !legacy_single_instance_.Acquire(product::kLegacySingleInstanceName)) {
    logger_.Warning(L"Second instance detected; exiting this process.");
    MessageBoxW(nullptr, L"O'Louie is already running.",
                product::kDisplayName,
                MB_OK | MB_ICONINFORMATION);
    return false;
  }

  default_settings_ = settings::AppSettings::Defaults(paths_);
  settings_file_path_ = paths_.settings / L"settings.json";
  const auto load_result = settings::LoadSettingsFile(
      settings_file_path_, default_settings_, &settings_);
  if (load_result.LoadedFromDisk()) {
    logger_.Info(load_result.message);
  } else if (load_result.status ==
             settings::SettingsLoadStatus::MissingUsingDefaults) {
    logger_.Info(load_result.message);
  } else {
    logger_.Warning(load_result.message);
  }
  if (path_migration.status == logging::RuntimePathMigrationStatus::Migrated &&
      load_result.LoadedFromDisk()) {
    std::filesystem::path rebased_output;
    if (logging::RebasePathFromLegacyRoot(
            settings_.output_directory, path_migration.legacy_root,
            path_migration.canonical_root, &rebased_output)) {
      settings_.output_directory = std::move(rebased_output);
      const auto saved =
          settings::SaveSettingsFileAtomic(settings_file_path_, settings_);
      if (saved.Succeeded()) {
        logger_.Info(L"Rebased the migrated recording output directory.");
      } else {
        logger_.Warning(
            L"The migrated output directory is active for this run but could "
            L"not be persisted: " + saved.message);
      }
    }
  }
  std::wstring settings_error;
  if (!settings::Validate(settings_, &settings_error)) {
    logger_.Error(settings_error);
    ShowStartupFailure(settings_error);
    return false;
  }

  if (!hidden_window_.Create(
          instance_, product::kDisplayName,
          [this](HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
            return HandleWindowMessage(window, message, wparam, lparam);
          })) {
    logger_.Error(L"Hidden message window creation failed.");
    ShowStartupFailure(L"Could not create the hidden message window.");
    return false;
  }

  tray_.SetCommandSink([this](AppCommand command) { HandleCommand(command); });
  if (!tray_.Create(hidden_window_.hwnd(), product::kDisplayName)) {
    logger_.Error(L"Tray icon creation failed.");
    ShowStartupFailure(L"Could not create the tray icon.");
    return false;
  }
  tray_.SetNotificationsEnabled(settings_.show_overlay_notifications);
  tray_.SetClipPresetDurations(
      settings_.clips.presets_seconds[0], settings_.clips.presets_seconds[1],
      settings_.clips.presets_seconds[2], settings_.clips.custom_seconds);

  hotkeys_.SetCommandSink(
      [this](AppCommand command) { HandleCommand(command); });

  std::wstring hotkey_error;
  if (!ConfigureHotkeys(settings_, &hotkey_error)) {
    logger_.Warning(L"Hotkey registration failed: " + hotkey_error);
    tray_.ShowInfo(L"O'Louie hotkeys unavailable", hotkey_error);
  }

  std::wstring executable_error;
  executable_path_ = win32::CurrentExecutablePath(&executable_error);
  if (executable_path_.empty()) {
    logger_.Warning(executable_error);
  } else if (!win32::SetStartupRegistration(
                 settings_.start_with_windows, executable_path_,
                 &executable_error)) {
    logger_.Warning(executable_error);
  }

  mic_monitor_.SetStateSink(
      [this](const audio::MicMonitorSnapshot&) {
        PostMicMonitorStateChanged();
      });

  settings_ui_.Configure(
      instance_,
      [this](const settings::AppSettings& candidate, std::wstring* error) {
        return ApplySettings(candidate, error);
      },
      [this](std::wstring_view message) {
        logger_.Warning(std::wstring(message));
      },
      [this](const audio::MicMonitorOptions& options) {
        return StartMicMonitor(options);
      },
      [this] { return mic_monitor_.Stop(); },
      [this] { return mic_monitor_.Snapshot(); });

  CreateVideoRecorder();
  CreateRecordingRecovery();

  if (load_result.UsedDefaults() &&
      load_result.status != settings::SettingsLoadStatus::MissingUsingDefaults) {
    tray_.ShowInfo(L"O'Louie settings", load_result.message);
  }
  if (path_migration.NeedsWarning()) {
    tray_.ShowInfo(L"O'Louie data migration", path_migration.message);
  }

  logger_.Info(L"O'Louie tray recorder shell is ready.");
  return true;
}

void AppHost::Shutdown() {
  if (shutdown_complete_) {
    return;
  }

  settings_ui_.Shutdown();
  mic_monitor_.SetStateSink({});
  mic_monitor_.Shutdown();
  if (recording_recovery_ != nullptr) {
    recording_recovery_->SetStateSink({});
    recording_recovery_->Shutdown();
    recording_recovery_.reset();
  }
  if (video_recorder_ != nullptr) {
    video_recorder_->Shutdown();
    video_recorder_.reset();
  }
  hotkeys_.UnregisterAll();
  tray_.Destroy();
  hidden_window_.Destroy();

  logger_.Info(L"Stopping O'Louie foundation shell.");
  logger_.Close();

  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }

  shutdown_complete_ = true;
}

void AppHost::HandleCommand(AppCommand command) {
  switch (command) {
    case AppCommand::OpenSettings:
      logger_.Info(L"Settings command received.");
      {
        RefreshDiagnosticsSnapshot();
        std::wstring error;
        if (!settings_ui_.Open(settings_, default_settings_, &error)) {
          logger_.Error(L"Settings window open failed: " + error);
          tray_.ShowInfo(L"O'Louie settings failed", error);
        }
      }
      break;
    case AppCommand::Exit:
      logger_.Info(L"Exit command received.");
      if (hidden_window_.hwnd() != nullptr) {
        DestroyWindow(hidden_window_.hwnd());
      }
      break;
    case AppCommand::ToggleRecording:
      logger_.Info(L"Toggle recording command received.");
      HandleToggleRecording();
      break;
    case AppCommand::SaveLast30Seconds:
      logger_.Info(L"Save first clip preset command received.");
      HandleSaveLastClip(settings_.clips.presets_seconds[0]);
      break;
    case AppCommand::SaveSecondPreset:
      logger_.Info(L"Save second clip preset command received.");
      HandleSaveLastClip(settings_.clips.presets_seconds[1]);
      break;
    case AppCommand::SaveLast5Minutes:
      logger_.Info(L"Save third clip preset command received.");
      HandleSaveLastClip(settings_.clips.presets_seconds[2]);
      break;
    case AppCommand::SaveCustomClip:
      logger_.Info(L"Save custom clip duration command received.");
      HandleSaveLastClip(settings_.clips.custom_seconds);
      break;
    case AppCommand::AddBookmark:
      logger_.Info(L"Bookmark command received.");
      HandleAddBookmark();
      break;
    case AppCommand::RecoverRecording:
      logger_.Info(L"Interrupted recording recovery command received.");
      HandleRecoverRecording();
      break;
    case AppCommand::DiscardRecovery:
      logger_.Info(L"Interrupted recording discard command received.");
      HandleDiscardRecovery();
      break;
  }
}

void AppHost::HandleRecoverRecording() {
  if (recording_recovery_ == nullptr) {
    tray_.ShowInfo(L"O'Louie recovery failed",
                   L"Recording recovery is unavailable.");
    return;
  }
  const auto command = recording_recovery_->ExportFirst();
  if (!command.Accepted()) {
    logger_.Warning(L"Recording recovery command rejected: " +
                    command.message);
    tray_.ShowInfo(L"O'Louie recovery", command.message);
  }
}

void AppHost::HandleDiscardRecovery() {
  if (recording_recovery_ == nullptr) {
    tray_.ShowInfo(L"O'Louie recovery failed",
                   L"Recording recovery is unavailable.");
    return;
  }
  const auto command = recording_recovery_->DiscardFirst();
  if (!command.Accepted()) {
    logger_.Warning(L"Recording discard command rejected: " +
                    command.message);
    tray_.ShowInfo(L"O'Louie recovery", command.message);
  }
}

void AppHost::HandleSaveLastClip(int preset_seconds) {
  if (video_recorder_ == nullptr || preset_seconds <= 0 ||
      (std::find(settings_.clips.presets_seconds.begin(),
                 settings_.clips.presets_seconds.end(), preset_seconds) ==
           settings_.clips.presets_seconds.end() &&
       preset_seconds != settings_.clips.custom_seconds)) {
    logger_.Error(L"Clip recorder configuration is unavailable.");
    tray_.ShowInfo(L"O'Louie clip failed",
                   L"Clip recorder configuration is unavailable.");
    return;
  }

  const auto duration = std::chrono::seconds(preset_seconds);
  const auto command = video_recorder_->SaveLastClip(
      std::chrono::duration_cast<std::chrono::milliseconds>(duration));
  if (!command.Accepted()) {
    logger_.Warning(L"Clip command rejected: " + command.message);
    tray_.ShowInfo(L"O'Louie clip command", command.message);
  }
}

void AppHost::HandleAddBookmark() {
  if (video_recorder_ == nullptr ||
      settings_.clips.bookmark_pre_seconds < 0 ||
      settings_.clips.bookmark_post_seconds < 0) {
    logger_.Error(L"Bookmark recorder configuration is unavailable.");
    tray_.ShowInfo(L"O'Louie bookmark failed",
                   L"Bookmark recorder configuration is unavailable.");
    return;
  }

  const auto pre_roll =
      std::chrono::seconds(settings_.clips.bookmark_pre_seconds);
  const auto post_roll =
      std::chrono::seconds(settings_.clips.bookmark_post_seconds);
  const auto command = video_recorder_->AddBookmarkAndSave(
      std::chrono::duration_cast<std::chrono::milliseconds>(pre_roll),
      std::chrono::duration_cast<std::chrono::milliseconds>(post_roll));
  if (!command.Accepted()) {
    logger_.Warning(L"Bookmark command rejected: " + command.message);
    tray_.ShowInfo(L"O'Louie bookmark command", command.message);
  }
}

void AppHost::HandleToggleRecording() {
  ReconfigureRecorderIfInactive();
  if (video_recorder_ == nullptr) {
    logger_.Error(L"Video recorder session is unavailable.");
    tray_.ShowInfo(L"O'Louie recording failed",
                   L"The video recorder session is unavailable.");
    return;
  }

  const auto snapshot = video_recorder_->Snapshot();
  record::VideoRecorderCommandResult command;
  if (snapshot.state == record::VideoRecorderState::Starting ||
      snapshot.state == record::VideoRecorderState::Recording) {
    recording_start_pending_after_mic_monitor_ = false;
    command = video_recorder_->StopAndSave();
  } else if (snapshot.state == record::VideoRecorderState::Stopping) {
    tray_.ShowInfo(L"O'Louie", L"The recording is already being saved.");
    return;
  } else {
    if (recording_start_pending_after_mic_monitor_) {
      recording_start_pending_after_mic_monitor_ = false;
      logger_.Info(L"Pending recording start canceled.");
      tray_.ShowInfo(L"O'Louie", L"Pending recording start canceled.");
      return;
    }
    const auto mic_monitor = mic_monitor_.Snapshot();
    if (mic_monitor.state == audio::MicMonitorState::Starting ||
        mic_monitor.state == audio::MicMonitorState::Monitoring ||
        mic_monitor.state == audio::MicMonitorState::Stopping) {
      recording_start_pending_after_mic_monitor_ = true;
      const auto stopped = mic_monitor_.Stop();
      if (!stopped.Accepted() &&
          stopped.status != audio::MicMonitorCommandStatus::NotRunning) {
        recording_start_pending_after_mic_monitor_ = false;
        logger_.Warning(L"Could not stop microphone check before recording: " +
                        stopped.message);
        tray_.ShowInfo(L"O'Louie recording", stopped.message);
        return;
      }
      logger_.Info(
          L"Stopping microphone check before starting the recording.");
      tray_.ShowInfo(
          L"O'Louie",
          L"Stopping microphone check before starting the recording.");
      return;
    }
    command = video_recorder_->Start();
  }

  if (!command.Accepted()) {
    logger_.Warning(L"Recording command rejected: " + command.message);
    tray_.ShowInfo(L"O'Louie recording command", command.message);
  }
}

void AppHost::PostRecorderStateChanged() {
  const HWND window = hidden_window_.hwnd();
  if (window != nullptr) {
    PostMessageW(window, kRecorderStateChangedMessage, 0, 0);
  }
}

void AppHost::PostMicMonitorStateChanged() {
  const HWND window = hidden_window_.hwnd();
  if (window != nullptr) {
    PostMessageW(window, kMicMonitorStateChangedMessage, 0, 0);
  }
}

void AppHost::HandleMicMonitorStateChanged() {
  const auto snapshot = mic_monitor_.Snapshot();
  if (snapshot.generation != last_mic_monitor_generation_ ||
      snapshot.state != last_mic_monitor_state_) {
    last_mic_monitor_generation_ = snapshot.generation;
    last_mic_monitor_state_ = snapshot.state;
    const std::wstring detail =
        std::wstring(L"Microphone check ") +
        audio::MicMonitorStateName(snapshot.state) + L": " +
        snapshot.message;
    if (snapshot.state == audio::MicMonitorState::Failed) {
      logger_.Error(detail);
    } else if (snapshot.using_fallback_output) {
      logger_.Warning(detail);
    } else {
      logger_.Info(detail);
    }
  }

  if (recording_start_pending_after_mic_monitor_ &&
      (snapshot.state == audio::MicMonitorState::Idle ||
       snapshot.state == audio::MicMonitorState::Failed)) {
    recording_start_pending_after_mic_monitor_ = false;
    logger_.Info(L"Microphone check stopped; starting pending recording.");
    HandleToggleRecording();
  }
}

void AppHost::PostRecoveryStateChanged() {
  const HWND window = hidden_window_.hwnd();
  if (window != nullptr) {
    PostMessageW(window, kRecoveryStateChangedMessage, 0, 0);
  }
}

void AppHost::HandleRecoveryStateChanged() {
  if (recording_recovery_ == nullptr) {
    return;
  }
  const auto snapshot = recording_recovery_->Snapshot();
  RefreshDiagnosticsSnapshot();
  const bool busy = snapshot.state == record::RecordingRecoveryState::Scanning ||
                    snapshot.state == record::RecordingRecoveryState::Exporting ||
                    snapshot.state == record::RecordingRecoveryState::Discarding;
  tray_.SetRecoveryState(snapshot.scan.ExportableCount(),
                         snapshot.scan.DiscardableCount(), busy);

  if (snapshot.generation != last_recovery_notification_generation_ ||
      snapshot.state != last_recovery_notification_state_) {
    last_recovery_notification_generation_ = snapshot.generation;
    last_recovery_notification_state_ = snapshot.state;
    if (snapshot.state == record::RecordingRecoveryState::Ready) {
      const std::wstring summary =
          L"Recovery scan completed: discovered=" +
          std::to_wstring(snapshot.scan.discovered_session_count) +
          L", scanned=" +
          std::to_wstring(snapshot.scan.scanned_session_count) +
          L", exportable=" +
          std::to_wstring(snapshot.scan.ExportableCount()) +
          L", discardable=" +
          std::to_wstring(snapshot.scan.DiscardableCount()) +
          L", truncated=" + (snapshot.scan.truncated ? L"true" : L"false") +
          L".";
      logger_.Info(summary);
      for (const auto& candidate : snapshot.scan.candidates) {
        const std::wstring facts =
            L"Recovery candidate: kind=" +
            std::wstring(record::RecordingRecoveryKindName(candidate.kind)) +
            L", session='" + candidate.session_directory.wstring() +
            L"', manifest_version=" +
            std::to_wstring(candidate.manifest_version) +
            L", packets=" + std::to_wstring(candidate.packet_count) +
            L", recovered_bytes=" +
            std::to_wstring(candidate.recovered_packet_bytes) +
            L", trailing_bytes=" +
            std::to_wstring(candidate.trailing_packet_bytes) + L". " +
            candidate.message;
        if (candidate.kind == record::RecordingRecoveryKind::Corrupt ||
            (candidate.kind ==
                 record::RecordingRecoveryKind::IncompleteMetadata &&
             !candidate.can_export)) {
          logger_.Warning(facts);
        } else {
          logger_.Info(facts);
        }
      }
      const size_t exportable = snapshot.scan.ExportableCount();
      const size_t discardable = snapshot.scan.DiscardableCount();
      if (snapshot.action_generation == 0 && discardable != 0) {
        const std::wstring message =
            exportable != 0
                ? L"Found " + std::to_wstring(exportable) +
                      (exportable == 1 ? L" recoverable recording."
                                       : L" recoverable recordings.") +
                      L" Use the tray menu to recover or discard."
                : L"Found " + std::to_wstring(discardable) +
                      (discardable == 1
                           ? L" interrupted recording that cannot be recovered."
                           : L" interrupted recordings that cannot be recovered.") +
                      L" Use the tray menu to move them to discarded storage.";
        tray_.ShowInfo(L"O'Louie interrupted recording", message);
      }
    } else if (snapshot.state == record::RecordingRecoveryState::Failed) {
      logger_.Error(L"Recovery scan failed: " + snapshot.message);
      tray_.ShowInfo(L"O'Louie recovery scan failed", snapshot.message);
    }
  }

  if (snapshot.action_generation != last_recovery_action_generation_) {
    last_recovery_action_generation_ = snapshot.action_generation;
    if (snapshot.action.Succeeded()) {
      logger_.Info(snapshot.action.message);
      const bool exported = snapshot.action_kind ==
                            record::RecordingRecoveryActionKind::Export;
      const auto& path = exported ? snapshot.action.output_path
                                  : snapshot.action.retained_session_path;
      tray_.ShowInfo(exported ? L"O'Louie recording recovered"
                              : L"O'Louie recording discarded",
                     snapshot.action.message + L" " + path.wstring());
    } else {
      logger_.Error(L"Recording recovery action failed: " +
                    snapshot.action.message);
      tray_.ShowInfo(L"O'Louie recovery failed", snapshot.action.message);
    }
  }
}

void AppHost::HandleRecorderStateChanged() {
  if (video_recorder_ == nullptr) {
    return;
  }
  const auto snapshot = video_recorder_->Snapshot();
  const bool recorder_changed =
      snapshot.generation != last_recorder_notification_generation_ ||
      snapshot.state != last_recorder_notification_state_;
  const bool clip_changed =
      snapshot.clip_event_generation != last_clip_notification_generation_;
  const bool diagnostics_changed =
      snapshot.diagnostics_generation !=
      last_recorder_diagnostics_generation_;
  if (diagnostics_changed || clip_changed || recorder_changed) {
    last_recorder_diagnostics_generation_ =
        snapshot.diagnostics_generation;
    RefreshDiagnosticsSnapshot();
  }
  if (!recorder_changed && !clip_changed) {
    return;
  }
  if (recorder_changed) {
    last_recorder_notification_generation_ = snapshot.generation;
    last_recorder_notification_state_ = snapshot.state;

    switch (snapshot.state) {
      case record::VideoRecorderState::Idle:
        tray_.SetRecordingState(win32::TrayRecordingState::Idle);
        if (!snapshot.message.empty()) {
          logger_.Info(snapshot.message);
          tray_.ShowInfo(L"O'Louie", snapshot.message);
        }
        break;
      case record::VideoRecorderState::Starting:
        tray_.SetRecordingState(win32::TrayRecordingState::Starting);
        logger_.Info(snapshot.message);
        tray_.ShowInfo(L"O'Louie",
                       L"Preparing recording sources and encoders.");
        break;
      case record::VideoRecorderState::Recording:
        tray_.SetRecordingState(win32::TrayRecordingState::Recording);
        logger_.Info(L"Recording started.");
        tray_.ShowInfo(L"O'Louie", L"Recording started.");
        break;
      case record::VideoRecorderState::Stopping:
        tray_.SetRecordingState(win32::TrayRecordingState::Stopping);
        logger_.Info(snapshot.message);
        tray_.ShowInfo(L"O'Louie",
                       L"Recording stopped; saving full recording.");
        break;
      case record::VideoRecorderState::Saved: {
        tray_.SetRecordingState(win32::TrayRecordingState::Idle);
        const std::wstring output = snapshot.output_path.wstring();
        logger_.Info(L"Recording saved: " + output);
        tray_.ShowInfo(L"O'Louie recording saved", L"Saved: " + output);
        break;
      }
      case record::VideoRecorderState::Failed:
        tray_.SetRecordingState(win32::TrayRecordingState::Idle);
        logger_.Error(snapshot.message);
        tray_.ShowInfo(snapshot.recording_saved_after_failure
                           ? L"O'Louie recording stopped and saved"
                           : L"O'Louie recording failed",
                       snapshot.message);
        break;
    }
  }

  if (clip_changed) {
    last_clip_notification_generation_ = snapshot.clip_event_generation;
    switch (snapshot.clip.state) {
      case record::VideoRecorderClipState::None:
        break;
      case record::VideoRecorderClipState::Queued: {
        const auto seconds = snapshot.clip.duration.count() / 1000;
        if (snapshot.clip.kind ==
            record::VideoRecorderExportKind::Bookmark) {
          if (snapshot.clip.bookmark_id == 0) {
            logger_.Info(L"Bookmark request queued: request " +
                         std::to_wstring(snapshot.clip.request_id));
          } else {
            const std::wstring message =
                L"Bookmark " + std::to_wstring(snapshot.clip.bookmark_id) +
                L" added; saving its clip.";
            logger_.Info(L"Bookmark added; export queued: bookmark " +
                         std::to_wstring(snapshot.clip.bookmark_id));
            tray_.ShowInfo(L"O'Louie bookmark added", message);
          }
        } else {
          const std::wstring message =
              L"Saving the last " + std::to_wstring(seconds) + L" seconds.";
          logger_.Info(L"Clip queued: request " +
                       std::to_wstring(snapshot.clip.request_id));
          tray_.ShowInfo(L"O'Louie clip queued", message);
        }
        break;
      }
      case record::VideoRecorderClipState::Saved: {
        const std::wstring output = snapshot.clip.output_path.wstring();
        if (snapshot.clip.kind ==
            record::VideoRecorderExportKind::Bookmark) {
          logger_.Info(L"Bookmark clip saved: " + output);
          tray_.ShowInfo(L"O'Louie bookmark saved", L"Saved: " + output);
        } else {
          logger_.Info(L"Clip saved: " + output);
          tray_.ShowInfo(L"O'Louie clip saved", L"Saved: " + output);
        }
        break;
      }
      case record::VideoRecorderClipState::Failed:
        if (snapshot.clip.kind ==
            record::VideoRecorderExportKind::Bookmark) {
          logger_.Error(L"Bookmark export failed: " + snapshot.clip.message);
          tray_.ShowInfo(L"O'Louie bookmark failed", snapshot.clip.message);
        } else {
          logger_.Error(L"Clip failed: " + snapshot.clip.message);
          tray_.ShowInfo(L"O'Louie clip failed", snapshot.clip.message);
        }
        break;
    }
  }

  ReconfigureRecorderIfInactive();
}

bool AppHost::ApplySettings(const settings::AppSettings& candidate,
                            std::wstring* error) {
  std::wstring validation_error;
  if (!settings::Validate(candidate, &validation_error)) {
    if (error != nullptr) {
      *error = std::move(validation_error);
    }
    return false;
  }

  const settings::AppSettings previous = settings_;
  std::wstring hotkey_error;
  if (!ConfigureHotkeys(candidate, &hotkey_error)) {
    if (error != nullptr) {
      *error = std::move(hotkey_error);
    }
    return false;
  }

  std::wstring startup_error;
  if (!win32::SetStartupRegistration(candidate.start_with_windows,
                                     executable_path_, &startup_error)) {
    std::wstring rollback_error;
    ConfigureHotkeys(previous, &rollback_error);
    if (error != nullptr) {
      *error = startup_error;
      if (!rollback_error.empty()) {
        *error += L" " + rollback_error;
      }
    }
    return false;
  }

  const auto save =
      settings::SaveSettingsFileAtomic(settings_file_path_, candidate);
  if (!save.Succeeded()) {
    std::wstring ignored;
    win32::SetStartupRegistration(previous.start_with_windows,
                                  executable_path_, &ignored);
    std::wstring rollback_error;
    ConfigureHotkeys(previous, &rollback_error);
    if (error != nullptr) {
      *error = save.message;
      if (!rollback_error.empty()) {
        *error += L" " + rollback_error;
      }
    }
    return false;
  }

  settings_ = candidate;
  RefreshDiagnosticsSnapshot();
  tray_.SetNotificationsEnabled(settings_.show_overlay_notifications);
  tray_.SetClipPresetDurations(
      settings_.clips.presets_seconds[0], settings_.clips.presets_seconds[1],
      settings_.clips.presets_seconds[2], settings_.clips.custom_seconds);

  if (video_recorder_ != nullptr) {
    const auto state = video_recorder_->Snapshot().state;
    recorder_reconfigure_pending_ =
        state == record::VideoRecorderState::Starting ||
        state == record::VideoRecorderState::Recording ||
        state == record::VideoRecorderState::Stopping;
  }
  if (!recorder_reconfigure_pending_) {
    CreateVideoRecorder();
  }
  logger_.Info(L"Settings saved and applied.");
  return true;
}

audio::MicMonitorCommandResult AppHost::StartMicMonitor(
    const audio::MicMonitorOptions& options) {
  if (recording_start_pending_after_mic_monitor_) {
    return {audio::MicMonitorCommandStatus::AlreadyRunning,
            L"A recording is waiting to start."};
  }
  if (video_recorder_ != nullptr) {
    const auto state = video_recorder_->Snapshot().state;
    if (state == record::VideoRecorderState::Starting ||
        state == record::VideoRecorderState::Recording ||
        state == record::VideoRecorderState::Stopping) {
      return {audio::MicMonitorCommandStatus::AlreadyRunning,
              L"Mic check is unavailable while recording is active."};
    }
  }
  return mic_monitor_.Start(options);
}

bool AppHost::ConfigureHotkeys(const settings::AppSettings& settings,
                               std::wstring* error) {
  std::vector<win32::HotkeyBinding> bindings;
  if (!BuildHotkeyBindings(settings, &bindings, error)) {
    return false;
  }
  return hotkeys_.ReplaceAll(hidden_window_.hwnd(), bindings, error);
}

void AppHost::CreateVideoRecorder() {
  if (video_recorder_ != nullptr) {
    video_recorder_->SetStateSink({});
    video_recorder_->Shutdown();
  }
  video_recorder_ = std::make_unique<record::VideoRecorderSession>(
      BuildVideoRecorderOptions(paths_, settings_));
  video_recorder_->SetStateSink(
      [this](const record::VideoRecorderSnapshot&) {
        PostRecorderStateChanged();
      });
  recorder_reconfigure_pending_ = false;
  RefreshDiagnosticsSnapshot();
}

void AppHost::CreateRecordingRecovery() {
  if (recording_recovery_ != nullptr) {
    recording_recovery_->SetStateSink({});
    recording_recovery_->Shutdown();
  }
  record::RecordingRecoveryScanOptions options;
  options.session_root_directory = paths_.sessions;
  options.output_directory = settings_.output_directory;
  recording_recovery_ =
      std::make_unique<record::RecordingRecoverySession>(std::move(options));
  recording_recovery_->SetStateSink(
      [this](const record::RecordingRecoverySnapshot&) {
        PostRecoveryStateChanged();
      });
  const auto started = recording_recovery_->StartScan();
  if (!started.Accepted()) {
    logger_.Warning(L"Could not start recording recovery scan: " +
                    started.message);
  }
  RefreshDiagnosticsSnapshot();
}

void AppHost::RefreshDiagnosticsSnapshot() {
  record::VideoRecorderSnapshot recorder;
  record::RecordingRecoverySnapshot recovery;
  if (video_recorder_ != nullptr) {
    recorder = video_recorder_->Snapshot();
  }
  if (recording_recovery_ != nullptr) {
    recovery = recording_recovery_->Snapshot();
  }
  auto snapshot =
      diagnostics::BuildDiagnosticsSnapshot(settings_, recorder, recovery);
  if ((settings_ui_.visible() || ExerciseDiagnosticsLoggingEnabled()) &&
      snapshot.generation != last_logged_diagnostics_generation_) {
    last_logged_diagnostics_generation_ = snapshot.generation;
    const auto& stats = snapshot.recorder_stats;
    const auto packet_bearing_audio_tracks = std::count_if(
        snapshot.audio_tracks.begin(), snapshot.audio_tracks.end(),
        [](const diagnostics::DiagnosticsAudioTrack& track) {
          return track.packet_bearing;
        });
    const auto applied_encoder_controls = std::count_if(
        stats.runtime.encoder_codec_settings.begin(),
        stats.runtime.encoder_codec_settings.end(),
        [](const encode::MfHardwareH264CodecSettingResult& setting) {
          return setting.applied;
        });
    const auto scheduled_audio_sources = std::count_if(
        stats.runtime.audio_capture_scheduling.begin(),
        stats.runtime.audio_capture_scheduling.end(),
        [](const performance::MultimediaThreadSchedulingSnapshot& scheduling) {
          return scheduling.Succeeded();
        });
    logger_.Info(
        L"Diagnostics snapshot: recorder=" +
        std::wstring(record::VideoRecorderStateName(snapshot.recorder_state)) +
        L", captured=" + std::to_wstring(stats.captured_frame_count) +
        L", accepted=" + std::to_wstring(stats.accepted_frame_count) +
        L", rate_limited=" +
        std::to_wstring(stats.rate_limited_frame_count) +
        L", dropped=" + std::to_wstring(stats.dropped_frame_count) +
        L", encoded=" + std::to_wstring(stats.encoded_frame_count) +
        L", discarded=" +
        std::to_wstring(stats.discarded_video_frame_count) +
        L", video_queue_depth=" +
        std::to_wstring(stats.runtime.queued_video_frame_count) +
        L", video_queue_peak=" +
        std::to_wstring(stats.runtime.peak_queued_video_frame_count) +
        L", video_queue_overflows=" +
        std::to_wstring(stats.runtime.video_queue_overflow_event_count) +
        L", video_queue_backlog_drops=" +
        std::to_wstring(stats.runtime.video_queue_dropped_backlog_count) +
        L", capture_texture_pool_reuses=" +
        std::to_wstring(
            stats.runtime.video_texture_pool.reused_texture_count) +
        L", capture_texture_pool_exhausted=" +
        std::to_wstring(
            stats.runtime.video_texture_pool_exhausted_frame_count) +
        L", capture_copy_max_ns=" +
        std::to_wstring(
            stats.runtime.video_capture_copy_maximum_latency_ns) +
        L", video_queue_wait_max_ns=" +
        std::to_wstring(stats.runtime.video_queue_wait_maximum_ns) +
        L", conversion_submit_max_ns=" +
        std::to_wstring(stats.runtime.video_converter
                            .maximum_conversion_submission_latency_ns) +
        L", encoder_wait_max_ns=" +
        std::to_wstring(stats.runtime.video_encoder_wait_maximum_ns) +
        L", performance_mode=" +
        std::wstring(performance::CapturePerformanceModeName(
            stats.runtime.performance_mode)) +
        L", recorder_mmcss=" +
        std::to_wstring(stats.runtime.recorder_scheduling.Succeeded()) +
        L", capture_mmcss=" +
        std::to_wstring(stats.runtime.capture_scheduling.Succeeded()) +
        L", video_encode_mmcss=" +
        std::to_wstring(stats.runtime.video_encode_scheduling.Succeeded()) +
        L", realtime_process_priority=" +
        std::to_wstring(stats.runtime.realtime_process_priority) +
        L", applied_encoder_controls=" +
        std::to_wstring(applied_encoder_controls) +
        L", scheduled_audio_sources=" +
        std::to_wstring(scheduled_audio_sources) +
        L", packet_writer_queue_peak=" +
        std::to_wstring(
            stats.runtime.packet_writer.peak_queued_packet_count) +
        L", packet_write_max_ns=" +
        std::to_wstring(
            stats.runtime.packet_writer.maximum_write_latency_ns) +
        L", captured_audio_packets=" +
        std::to_wstring(stats.captured_audio_packet_count) +
        L", captured_audio_frames=" +
        std::to_wstring(stats.captured_audio_frame_count) +
        L", encoded_audio_packets=" +
        std::to_wstring(stats.encoded_audio_packet_count) +
        L", queued_audio_blocks=" +
        std::to_wstring(stats.runtime.queued_audio_block_count) +
        L", peak_queued_audio_blocks=" +
        std::to_wstring(stats.runtime.peak_queued_audio_block_count) +
        L", audio_queue_capacity_per_track=" +
        std::to_wstring(stats.runtime.audio_queue_capacity_per_track) +
        L", audio_tracks=" + std::to_wstring(snapshot.audio_tracks.size()) +
        L", packet_bearing_audio_tracks=" +
        std::to_wstring(packet_bearing_audio_tracks) +
        L", observed_fps=" + std::to_wstring(snapshot.observed_fps) +
        L", observed_bitrate_bps=" +
        std::to_wstring(snapshot.observed_bitrate_bps) +
        L", pending_commands=" +
        std::to_wstring(stats.runtime.pending_export_command_count) +
        L", outstanding_exports=" +
        std::to_wstring(stats.runtime.outstanding_export_count) +
        L", submitted_exports=" +
        std::to_wstring(stats.runtime.submitted_export_count) +
        L", saved_exports=" +
        std::to_wstring(stats.runtime.saved_export_count) +
        L", failed_exports=" +
        std::to_wstring(stats.runtime.failed_export_count) +
        L", rejected_exports=" +
        std::to_wstring(stats.runtime.rejected_export_count) + L".");
  }
  settings_ui_.SetDiagnosticsSnapshot(std::move(snapshot));
}

void AppHost::ReconfigureRecorderIfInactive() {
  if (!recorder_reconfigure_pending_ || video_recorder_ == nullptr) {
    return;
  }
  const auto state = video_recorder_->Snapshot().state;
  if (state == record::VideoRecorderState::Starting ||
      state == record::VideoRecorderState::Recording ||
      state == record::VideoRecorderState::Stopping) {
    return;
  }
  CreateVideoRecorder();
  logger_.Info(L"Saved settings are ready for the next recording.");
}

LRESULT AppHost::HandleWindowMessage(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
  if (tray_.HandleMessage(message, wparam, lparam)) {
    return 0;
  }

  if (hotkeys_.HandleMessage(message, wparam, lparam)) {
    return 0;
  }

  if (message == kRecorderStateChangedMessage) {
    HandleRecorderStateChanged();
    return 0;
  }

  if (message == kRecoveryStateChangedMessage) {
    HandleRecoveryStateChanged();
    return 0;
  }

  if (message == kMicMonitorStateChangedMessage) {
    HandleMicMonitorStateChanged();
    return 0;
  }

  switch (message) {
    case WM_COMMAND:
      if (tray_.HandleCommand(wparam)) {
        return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_QUERYENDSESSION:
      logger_.Info(L"Windows session shutdown requested.");
      return TRUE;
  }

  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace olouie::app
