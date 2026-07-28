#include <chrono>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/BgraTexturePool.h"
#include "capture/CapturedVideoFrameSink.h"
#include "capture/VideoFrameCadence.h"
#include "capture/VideoFrameQueue.h"
#include "capture/WgcMonitorCaptureSession.h"
#include "encode/VideoCaptureEncodeBridge.h"
#include "encode/VideoEncodeChain.h"
#include "encode/VideoEncodeWorker.h"
#include "encode/VideoLiveCaptureEncode.h"
#include "graphics/D3D11DeviceContext.h"
#include "graphics/D3D11DeviceFault.h"
#include "graphics/DisplayManager.h"
#include "graphics/GpuBgraToNv12.h"
#include "logging/RuntimePathMigration.h"
#include "logging/RuntimePaths.h"
#include "record/Timebase.h"
#include "settings/HotkeyBinding.h"
#include "settings/Settings.h"
#include "settings/SettingsStore.h"
#include "ui/ImGuiDx11Host.h"
#include "win32/HotkeyManager.h"
#include "win32/WindowAffinity.h"

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

LRESULT CALLBACK AffinitySmokeWindowProc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

bool VerifyCaptureExclusionHelper(std::wstring* error) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  constexpr wchar_t kClassName[] = L"O'LouieAffinitySmokeWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = AffinitySmokeWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kClassName;

  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    if (error != nullptr) {
      *error = L"RegisterClassExW failed for affinity smoke.";
    }
    return false;
  }

  HWND window =
      CreateWindowExW(0, kClassName, L"O'Louie Affinity Smoke",
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320,
                      200, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    if (error != nullptr) {
      *error = L"CreateWindowExW failed for affinity smoke.";
    }
    UnregisterClassW(kClassName, instance);
    return false;
  }

  std::wstring affinity_error;
  const bool excluded =
      olouie::win32::TryExcludeFromCapture(window, &affinity_error);

  DestroyWindow(window);
  UnregisterClassW(kClassName, instance);

  if (!excluded && error != nullptr) {
    *error = affinity_error;
  }

  return excluded;
}

bool SameSettings(const olouie::settings::AppSettings& left,
                  const olouie::settings::AppSettings& right) {
  return left.output_directory == right.output_directory &&
         left.start_with_windows == right.start_with_windows &&
         left.show_overlay_notifications ==
             right.show_overlay_notifications &&
         left.video.monitor_device_name == right.video.monitor_device_name &&
         left.video.resolution_mode == right.video.resolution_mode &&
         left.video.custom_width == right.video.custom_width &&
         left.video.custom_height == right.video.custom_height &&
         left.video.fps == right.video.fps &&
         left.video.bitrate_mbps == right.video.bitrate_mbps &&
         left.video.gop_seconds == right.video.gop_seconds &&
         left.video.capture_cursor == right.video.capture_cursor &&
         left.video.encoder_backend == right.video.encoder_backend &&
         left.video.performance_mode == right.video.performance_mode &&
         left.audio.system_mix == right.audio.system_mix &&
         left.audio.mic == right.audio.mic &&
         left.audio.separate_tracks == right.audio.separate_tracks &&
         left.audio.default_mixed_track == right.audio.default_mixed_track &&
         left.audio.sample_rate == right.audio.sample_rate &&
         left.audio.mic_check_output_device_id ==
             right.audio.mic_check_output_device_id &&
         left.clips.presets_seconds == right.clips.presets_seconds &&
         left.clips.custom_seconds == right.clips.custom_seconds &&
         left.clips.bookmark_pre_seconds ==
             right.clips.bookmark_pre_seconds &&
         left.clips.bookmark_post_seconds ==
             right.clips.bookmark_post_seconds &&
         left.hotkeys.toggle_recording == right.hotkeys.toggle_recording &&
         left.hotkeys.save_last_30s == right.hotkeys.save_last_30s &&
         left.hotkeys.save_last_5m == right.hotkeys.save_last_5m &&
         left.hotkeys.bookmark == right.hotkeys.bookmark;
}

bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(output);
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool VerifySettingsPersistence(const olouie::logging::RuntimePaths& base_paths,
                               std::wstring* error) {
  const auto root = base_paths.root / L"settings-store";
  std::error_code filesystem_error;
  std::filesystem::remove_all(root, filesystem_error);
  const auto paths = olouie::logging::RuntimePaths::FromRoot(root);
  paths.EnsureCreated();
  const auto file = paths.settings / L"settings.json";
  const auto defaults = olouie::settings::AppSettings::Defaults(paths);

  olouie::settings::AppSettings loaded;
  auto load =
      olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (load.status !=
          olouie::settings::SettingsLoadStatus::MissingUsingDefaults ||
      !SameSettings(loaded, defaults)) {
    if (error != nullptr) {
      *error = L"Missing settings should use defaults.";
    }
    return false;
  }

  auto saved = defaults;
  saved.output_directory = paths.root / L"captures";
  saved.start_with_windows = true;
  saved.show_overlay_notifications = false;
  saved.video.monitor_device_name = L"\\\\.\\DISPLAY1";
  saved.video.resolution_mode = olouie::settings::ResolutionMode::Custom;
  saved.video.custom_width = 2560;
  saved.video.custom_height = 1440;
  saved.video.fps = 120;
  saved.video.bitrate_mbps = 48;
  saved.video.gop_seconds = 1.5;
  saved.video.capture_cursor = false;
  saved.video.performance_mode =
      olouie::performance::CapturePerformanceMode::CaptureFirst;
  saved.audio.mic = true;
  saved.audio.mic_check_output_device_id = L"{OLOUIE-TEST-OUTPUT}";
  saved.clips.presets_seconds = {15, 60, 180};
  saved.clips.custom_seconds = 90;
  saved.clips.bookmark_pre_seconds = 45;
  saved.hotkeys.toggle_recording = L"Ctrl+Alt+R";
  saved.hotkeys.save_last_30s = L"Ctrl+Alt+F8";
  saved.hotkeys.save_last_5m = L"Ctrl+Alt+F9";
  saved.hotkeys.bookmark = L"Ctrl+Alt+F10";

  const auto save = olouie::settings::SaveSettingsFileAtomic(file, saved);
  std::filesystem::path temporary = file;
  temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
  if (!save.Succeeded() || !std::filesystem::is_regular_file(file) ||
      std::filesystem::exists(temporary)) {
    if (error != nullptr) {
      *error = L"Atomic settings save did not publish cleanly: " + save.message;
    }
    return false;
  }

  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (!load.LoadedFromDisk() || !SameSettings(loaded, saved)) {
    if (error != nullptr) {
      *error = L"Settings persistence round trip changed data.";
    }
    return false;
  }

  std::string version_two = ReadTextFile(file);
  const size_t version_field = version_two.find("\"version\":3");
  const std::string performance_json =
      ",\"performance_mode\":\"capture_first\"";
  const size_t performance_field = version_two.find(performance_json);
  if (version_field == std::string::npos ||
      performance_field == std::string::npos) {
    if (error != nullptr) {
      *error = L"Could not derive the version-2 migration fixture.";
    }
    return false;
  }
  version_two.replace(version_field, std::string("\"version\":3").size(),
                      "\"version\":2");
  version_two.erase(performance_field, performance_json.size());
  if (!WriteTextFile(file, version_two)) {
    if (error != nullptr) {
      *error = L"Could not write the version-2 migration fixture.";
    }
    return false;
  }
  auto migrated_v2 = saved;
  migrated_v2.video.performance_mode =
      olouie::performance::CapturePerformanceMode::Balanced;
  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (!load.LoadedFromDisk() || !SameSettings(loaded, migrated_v2)) {
    if (error != nullptr) {
      *error = L"Version-2 settings did not migrate to Balanced mode.";
    }
    return false;
  }

  std::string version_one = version_two;
  const size_t version_two_field = version_one.find("\"version\":2");
  const size_t mic_output_field =
      version_one.find(",\"mic_check_output_device_id\":");
  if (version_two_field == std::string::npos ||
      mic_output_field == std::string::npos) {
    if (error != nullptr) {
      *error = L"Could not derive the version-1 migration fixture.";
    }
    return false;
  }
  version_one.replace(version_two_field, std::string("\"version\":2").size(),
                      "\"version\":1");
  const std::string mic_output_json =
      ",\"mic_check_output_device_id\":\"{OLOUIE-TEST-OUTPUT}\"";
  if (version_one.compare(mic_output_field, mic_output_json.size(),
                          mic_output_json) != 0) {
    if (error != nullptr) {
      *error = L"Version-1 migration fixture has unexpected audio output data.";
    }
    return false;
  }
  version_one.erase(mic_output_field, mic_output_json.size());
  if (!WriteTextFile(file, version_one)) {
    if (error != nullptr) {
      *error = L"Could not write the version-1 migration fixture.";
    }
    return false;
  }
  auto migrated = migrated_v2;
  migrated.audio.mic_check_output_device_id.clear();
  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (!load.LoadedFromDisk() || !SameSettings(loaded, migrated)) {
    if (error != nullptr) {
      *error = L"Version-1 settings did not migrate to Windows Default output.";
    }
    return false;
  }

  if (!WriteTextFile(file, "{not-json")) {
    if (error != nullptr) {
      *error = L"Could not write malformed settings fixture.";
    }
    return false;
  }
  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (load.status !=
          olouie::settings::SettingsLoadStatus::MalformedUsingDefaults ||
      !SameSettings(loaded, defaults)) {
    if (error != nullptr) {
      *error = L"Malformed settings should report and use defaults.";
    }
    return false;
  }

  const std::string unsupported = "{\"version\":99}";
  if (!WriteTextFile(file, unsupported)) {
    if (error != nullptr) {
      *error = L"Could not write unsupported settings fixture.";
    }
    return false;
  }
  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (load.status != olouie::settings::SettingsLoadStatus::
                         UnsupportedVersionUsingDefaults ||
      !SameSettings(loaded, defaults) || ReadTextFile(file) != unsupported) {
    if (error != nullptr) {
      *error = L"Unsupported settings should remain untouched and use defaults.";
    }
    return false;
  }

  if (!olouie::settings::SaveSettingsFileAtomic(file, saved).Succeeded()) {
    if (error != nullptr) {
      *error = L"Could not restore the valid settings fixture.";
    }
    return false;
  }
  std::string invalid = ReadTextFile(file);
  const size_t fps = invalid.find("\"fps\":120");
  if (fps == std::string::npos) {
    if (error != nullptr) {
      *error = L"Could not locate the persisted FPS field.";
    }
    return false;
  }
  invalid.replace(fps, std::string("\"fps\":120").size(), "\"fps\":0");
  if (!WriteTextFile(file, invalid)) {
    if (error != nullptr) {
      *error = L"Could not write invalid settings fixture.";
    }
    return false;
  }
  load = olouie::settings::LoadSettingsFile(file, defaults, &loaded);
  if (load.status !=
          olouie::settings::SettingsLoadStatus::InvalidUsingDefaults ||
      !SameSettings(loaded, defaults)) {
    if (error != nullptr) {
      *error = L"Invalid settings should report and use defaults.";
    }
    return false;
  }

  const auto invalid_save = saved;
  auto invalid_candidate = invalid_save;
  invalid_candidate.video.fps = 0;
  const std::string before_invalid_save = ReadTextFile(file);
  if (olouie::settings::SaveSettingsFileAtomic(file, invalid_candidate)
          .Succeeded() ||
      ReadTextFile(file) != before_invalid_save) {
    if (error != nullptr) {
      *error = L"Invalid settings save should not replace the current file.";
    }
    return false;
  }
  return true;
}

bool VerifyRuntimePathMigration(std::wstring* error) {
  const std::wstring suffix =
      L"-" + std::to_wstring(GetCurrentProcessId()) + L"-smoke";
  const std::wstring canonical_name = L"O'LouieMigrationTest" + suffix;
  const std::wstring legacy_name = L"LegacyMigrationTest" + suffix;
  const auto canonical =
      olouie::logging::RuntimePaths::ForLocalAppData(canonical_name).root;
  const auto legacy =
      olouie::logging::RuntimePaths::ForLocalAppData(legacy_name).root;
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(canonical, ignored);
    ignored.clear();
    std::filesystem::remove_all(legacy, ignored);
  };
  cleanup();

  std::filesystem::create_directories(legacy / L"settings");
  if (!WriteTextFile(legacy / L"settings" / L"settings.json",
                     "{\"version\":2}")) {
    cleanup();
    if (error != nullptr) {
      *error = L"Could not create the runtime migration fixture.";
    }
    return false;
  }

  auto result = olouie::logging::ResolveRuntimePathsWithLegacyMigration(
      canonical_name, legacy_name);
  if (result.status !=
          olouie::logging::RuntimePathMigrationStatus::Migrated ||
      result.paths.root != canonical || std::filesystem::exists(legacy) ||
      !std::filesystem::exists(canonical / L"settings" / L"settings.json")) {
    cleanup();
    if (error != nullptr) {
      *error = L"The legacy runtime root was not migrated atomically.";
    }
    return false;
  }

  std::filesystem::path rebased;
  if (!olouie::logging::RebasePathFromLegacyRoot(
          legacy / L"exports" / L"nested", legacy, canonical, &rebased) ||
      rebased != canonical / L"exports" / L"nested" ||
      olouie::logging::RebasePathFromLegacyRoot(
          canonical / L"exports", legacy, canonical, &rebased)) {
    cleanup();
    if (error != nullptr) {
      *error = L"Runtime path rebasing did not preserve path boundaries.";
    }
    return false;
  }

  std::filesystem::create_directories(legacy);
  result = olouie::logging::ResolveRuntimePathsWithLegacyMigration(
      canonical_name, legacy_name);
  if (result.status !=
          olouie::logging::RuntimePathMigrationStatus::RootsConflict ||
      result.paths.root != canonical || !std::filesystem::exists(legacy)) {
    cleanup();
    if (error != nullptr) {
      *error = L"Conflicting runtime roots should remain untouched.";
    }
    return false;
  }

  cleanup();
  std::filesystem::create_directories(legacy);
  HANDLE lock = CreateFileW(
      legacy.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (lock == INVALID_HANDLE_VALUE) {
    cleanup();
    if (error != nullptr) {
      *error = L"Could not lock the migration fixture directory.";
    }
    return false;
  }
  result = olouie::logging::ResolveRuntimePathsWithLegacyMigration(
      canonical_name, legacy_name);
  CloseHandle(lock);
  const bool fallback_ok =
      result.status ==
          olouie::logging::RuntimePathMigrationStatus::LegacyFallback &&
      result.paths.root == legacy && std::filesystem::exists(legacy) &&
      !std::filesystem::exists(canonical);
  cleanup();
  if (!fallback_ok) {
    if (error != nullptr) {
      *error = L"A blocked runtime migration should use the legacy root.";
    }
    return false;
  }
  return true;
}

bool VerifyHotkeyParsingAndTransactions(std::wstring* error) {
  const auto canonical = olouie::settings::ParseHotkey(L" shift + ctrl + f9 ");
  if (!canonical.Succeeded() ||
      canonical.hotkey.canonical_label != L"Ctrl+Shift+F9" ||
      canonical.hotkey.virtual_key != VK_F9 ||
      (canonical.hotkey.modifiers & MOD_NOREPEAT) == 0 ||
      olouie::settings::ParseHotkey(L"Ctrl+F12").Succeeded() ||
      olouie::settings::ParseHotkey(L"R").Succeeded()) {
    if (error != nullptr) {
      *error = L"Hotkey parsing/canonicalization changed unexpectedly.";
    }
    return false;
  }

  struct FakeRegistrationState {
    std::map<int, std::pair<UINT, UINT>> active;
    std::pair<UINT, UINT> conflict{};
    DWORD last_error = ERROR_SUCCESS;
  } state;

  olouie::win32::HotkeyRegistrationApi api;
  api.register_hotkey = [&state](HWND, int id, UINT modifiers,
                                 UINT virtual_key) {
    const auto key = std::make_pair(
        modifiers & ~static_cast<UINT>(MOD_NOREPEAT), virtual_key);
    if (state.conflict == key) {
      state.last_error = ERROR_HOTKEY_ALREADY_REGISTERED;
      return false;
    }
    state.active[id] = key;
    state.last_error = ERROR_SUCCESS;
    return true;
  };
  api.unregister_hotkey = [&state](HWND, int id) {
    state.active.erase(id);
    return true;
  };
  api.last_error = [&state] { return state.last_error; };

  olouie::win32::HotkeyManager manager(std::move(api));
  const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
  const std::vector<olouie::win32::HotkeyBinding> original = {
      {1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'R',
       olouie::app::AppCommand::ToggleRecording, L"Ctrl+Shift+R"},
      {2, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8,
       olouie::app::AppCommand::SaveLast30Seconds, L"Ctrl+Shift+F8"},
  };
  if (!manager.ReplaceAll(owner, original, error) ||
      manager.Bindings().size() != original.size()) {
    return false;
  }

  state.conflict = std::make_pair(static_cast<UINT>(MOD_ALT),
                                  static_cast<UINT>(VK_F9));
  const std::vector<olouie::win32::HotkeyBinding> conflicting = {
      {1, MOD_ALT | MOD_NOREPEAT, VK_F9,
       olouie::app::AppCommand::ToggleRecording, L"Alt+F9"},
      {2, MOD_ALT | MOD_NOREPEAT, VK_F10,
       olouie::app::AppCommand::SaveLast30Seconds, L"Alt+F10"},
  };
  std::wstring conflict_error;
  if (manager.ReplaceAll(owner, conflicting, &conflict_error) ||
      conflict_error.find(L"Previous hotkeys remain active") ==
          std::wstring::npos ||
      manager.Bindings().size() != original.size() || state.active.size() != 2 ||
      state.active[1] != std::make_pair(static_cast<UINT>(MOD_CONTROL | MOD_SHIFT),
                                        static_cast<UINT>('R'))) {
    if (error != nullptr) {
      *error = L"Hotkey conflicts should restore the previous working bindings.";
    }
    return false;
  }

  olouie::app::AppCommand command = olouie::app::AppCommand::OpenSettings;
  manager.SetCommandSink(
      [&command](olouie::app::AppCommand value) { command = value; });
  if (!manager.HandleMessage(WM_HOTKEY, 1, 0) ||
      command != olouie::app::AppCommand::ToggleRecording) {
    if (error != nullptr) {
      *error = L"Restored hotkeys should still dispatch their original command.";
    }
    return false;
  }
  return true;
}

bool VerifyImGuiHostLifecycle(std::wstring* error) {
  olouie::ui::EventDrivenRenderState state;
  if (state.TryBeginFrame() || state.stats().hidden_request_count != 1) {
    if (error != nullptr) {
      *error = L"Hidden render state should suppress frame starts.";
    }
    return false;
  }
  state.SetVisible(true);
  if (!state.TryBeginFrame() || state.stats().rendered_frame_count != 1) {
    if (error != nullptr) {
      *error = L"Visible render state should permit frame starts.";
    }
    return false;
  }
  state.SetVisible(false);
  if (state.TryBeginFrame() || state.stats().rendered_frame_count != 1 ||
      state.stats().hidden_request_count != 2) {
    if (error != nullptr) {
      *error = L"Hidden render state should keep the rendered count stable.";
    }
    return false;
  }

  uint64_t rendered_callbacks = 0;
  std::wstring diagnostic;
  olouie::ui::ImGuiDx11Host host;
  if (!host.Create(
          GetModuleHandleW(nullptr), L"O'Louie Settings Host Test",
          [&rendered_callbacks] { ++rendered_callbacks; }, [] {},
          [&diagnostic](std::wstring_view message) {
            diagnostic = std::wstring(message);
          },
          error)) {
    return false;
  }
  if (host.visible() || host.stats().render.rendered_frame_count != 0) {
    if (error != nullptr) {
      *error = L"New settings host should be dormant while hidden.";
    }
    return false;
  }
  if (!host.Show(error) || rendered_callbacks != 1 ||
      host.stats().render.rendered_frame_count != 1) {
    return false;
  }
  host.Hide();
  const uint64_t before_hidden_timer =
      host.stats().render.rendered_frame_count;
  SendMessageW(host.hwnd(), WM_TIMER, 1, 0);
  const auto hidden_stats = host.stats();
  if (host.visible() ||
      hidden_stats.render.rendered_frame_count != before_hidden_timer ||
      hidden_stats.render.hidden_request_count == 0) {
    if (error != nullptr) {
      *error = L"Hidden settings timer requests must not render frames.";
    }
    return false;
  }
  host.Destroy();
  if (host.hwnd() != nullptr || host.created()) {
    if (error != nullptr) {
      *error = L"Settings host did not release its window lifetime.";
    }
    return false;
  }
  return true;
}

bool CreateQueueTestTexture(ID3D11Device* device,
                            winrt::com_ptr<ID3D11Texture2D>* texture,
                            std::wstring* error) {
  if (device == nullptr || texture == nullptr) {
    if (error != nullptr) {
      *error = L"Queue test texture creation needs a D3D11 device.";
    }
    return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 4;
  desc.Height = 4;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

  *texture = nullptr;
  const HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture->put());
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = L"Could not create queue test texture.";
    }
    return false;
  }
  return true;
}

olouie::capture::OwnedVideoFrame MakeQueueTestFrame(
    winrt::com_ptr<ID3D11Texture2D> texture,
    int64_t timestamp_ticks) {
  olouie::capture::OwnedVideoFrame frame;
  frame.texture = std::move(texture);
  frame.width = 4;
  frame.height = 4;
  frame.timestamp_ticks = timestamp_ticks;
  return frame;
}

olouie::capture::OwnedVideoFrame MakePooledQueueTestFrame(
    olouie::capture::BgraTexturePoolLease lease,
    int64_t timestamp_ticks) {
  olouie::capture::OwnedVideoFrame frame;
  D3D11_TEXTURE2D_DESC desc{};
  lease.get()->GetDesc(&desc);
  frame.texture.copy_from(lease.get());
  frame.texture_pool_lease = std::move(lease);
  frame.width = desc.Width;
  frame.height = desc.Height;
  frame.timestamp_ticks = timestamp_ticks;
  return frame;
}

olouie::capture::CapturedVideoFrameSinkResult CapturedVideoFrameResult(
    olouie::capture::CapturedVideoFrameSinkStatus status) {
  olouie::capture::CapturedVideoFrameSinkResult result;
  result.status = status;
  return result;
}

class FakeCapturedVideoFrameSink final
    : public olouie::capture::ICapturedVideoFrameSink {
 public:
  olouie::capture::CapturedVideoFrameSinkResult OnCapturedVideoFrame(
      olouie::capture::OwnedVideoFrame frame) override {
    ++received_frame_count;
    last_frame = std::move(frame);
    return result;
  }

  uint32_t received_frame_count = 0;
  olouie::capture::OwnedVideoFrame last_frame;
  olouie::capture::CapturedVideoFrameSinkResult result =
      CapturedVideoFrameResult(
          olouie::capture::CapturedVideoFrameSinkStatus::Success);
};

enum class FakeVideoFrameCopyMode {
  NoFrames,
  OneFrame,
  CaptureError,
};

FakeVideoFrameCopyMode g_fake_video_frame_copy_mode =
    FakeVideoFrameCopyMode::NoFrames;

olouie::capture::WgcFrameCopySmokeResult FakeVideoFrameCopyRunner(
    HMONITOR monitor,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::chrono::milliseconds duration,
    uint32_t max_copied_frames,
    olouie::capture::ICapturedVideoFrameSink* sink) {
  olouie::capture::WgcFrameCopySmokeResult result;
  result.supported = true;

  if (monitor == nullptr || device == nullptr || context == nullptr ||
      duration.count() <= 0 || max_copied_frames == 0) {
    result.error = L"Fake video frame copy runner received invalid input.";
    return result;
  }

  if (g_fake_video_frame_copy_mode == FakeVideoFrameCopyMode::CaptureError) {
    result.error = L"fake capture failure";
    return result;
  }

  if (g_fake_video_frame_copy_mode == FakeVideoFrameCopyMode::OneFrame) {
    winrt::com_ptr<ID3D11Texture2D> texture;
    std::wstring error;
    if (!CreateQueueTestTexture(device, &texture, &error)) {
      result.error = error;
      return result;
    }

    result.frame_count = 1;
    result.first_timestamp_ticks = 1000;
    result.last_timestamp_ticks = 1000;
    auto delivered = olouie::capture::DispatchCapturedVideoFrame(
        sink, MakeQueueTestFrame(std::move(texture), 1000));
    if (!delivered.Succeeded()) {
      result.error = delivered.message.empty()
                         ? L"Fake video frame sink rejected a frame."
                         : delivered.message;
      return result;
    }
    if (delivered.Accepted()) {
      result.copied_frame_count = 1;
    }
    if (delivered.Dropped()) {
      result.dropped_frame_count = 1;
    }
  }

  return result;
}

bool VerifyVideoFrameCadence(std::wstring* error) {
  using olouie::capture::VideoFrameCadence;
  using olouie::capture::VideoFrameCadenceConfig;
  using olouie::capture::VideoFrameCadenceDecision;

  constexpr uint64_t kFrequency = 10000000;
  for (const uint32_t source_fps : {60u, 120u, 144u, 240u}) {
    VideoFrameCadence cadence(
        VideoFrameCadenceConfig{kFrequency, 60, 1});
    for (uint64_t frame_index = 0; frame_index < source_fps;
         ++frame_index) {
      const uint64_t timestamp =
          (frame_index * kFrequency + source_fps / 2u) / source_fps;
      (void)cadence.Evaluate(static_cast<int64_t>(timestamp));
    }
    const auto stats = cadence.stats();
    if (stats.evaluated_frame_count != source_fps ||
        stats.accepted_frame_count != 60 ||
        stats.rate_limited_frame_count != source_fps - 60u ||
        stats.invalid_timestamp_count != 0 ||
        stats.invalid_config_count != 0) {
      if (error != nullptr) {
        *error = L"60 FPS cadence produced incorrect counts for a " +
                 std::to_wstring(source_fps) + L" FPS source.";
      }
      return false;
    }
  }

  VideoFrameCadence delayed(VideoFrameCadenceConfig{1000, 10, 1});
  if (delayed.Evaluate(0) != VideoFrameCadenceDecision::Accepted ||
      delayed.Evaluate(25) != VideoFrameCadenceDecision::RateLimited ||
      delayed.Evaluate(100) != VideoFrameCadenceDecision::Accepted ||
      delayed.Evaluate(1000) != VideoFrameCadenceDecision::Accepted ||
      delayed.Evaluate(1001) != VideoFrameCadenceDecision::RateLimited ||
      delayed.Evaluate(1100) != VideoFrameCadenceDecision::Accepted ||
      delayed.stats().delayed_rebase_count != 1) {
    if (error != nullptr) {
      *error = L"Delayed frame cadence should rebase without a catch-up burst.";
    }
    return false;
  }

  VideoFrameCadence invalid;
  if (invalid.Configure(VideoFrameCadenceConfig{100, 200, 1}) ||
      invalid.Evaluate(0) != VideoFrameCadenceDecision::InvalidConfig ||
      invalid.stats().invalid_config_count != 1) {
    if (error != nullptr) {
      *error = L"Frame cadence should reject a target faster than its clock.";
    }
    return false;
  }

  VideoFrameCadence timestamps(VideoFrameCadenceConfig{1000, 10, 1});
  if (timestamps.Evaluate(100) != VideoFrameCadenceDecision::Accepted ||
      timestamps.Evaluate(90) !=
          VideoFrameCadenceDecision::InvalidTimestamp ||
      timestamps.stats().invalid_timestamp_count != 1) {
    if (error != nullptr) {
      *error = L"Frame cadence should reject non-monotonic timestamps.";
    }
    return false;
  }
  timestamps.Reset();
  if (timestamps.stats().evaluated_frame_count != 0 ||
      timestamps.Evaluate(90) != VideoFrameCadenceDecision::Accepted ||
      std::wstring(olouie::capture::VideoFrameCadenceDecisionName(
          VideoFrameCadenceDecision::RateLimited)) != L"rate limited") {
    if (error != nullptr) {
      *error = L"Frame cadence reset or decision naming is incorrect.";
    }
    return false;
  }

  return true;
}

bool VerifyBgraTexturePool(ID3D11Device* device, std::wstring* error) {
  using olouie::capture::BgraTexturePool;
  using olouie::capture::BgraTexturePoolAcquireStatus;
  using olouie::capture::BgraTexturePoolConfig;

  BgraTexturePool pool;
  if (pool.Acquire().status !=
      BgraTexturePoolAcquireStatus::NotInitialized) {
    if (error != nullptr) {
      *error = L"Uninitialized BGRA texture pool should reject acquisition.";
    }
    return false;
  }

  std::wstring initialize_error;
  if (!pool.Initialize(device, BgraTexturePoolConfig{64, 48, 2},
                       &initialize_error)) {
    if (error != nullptr) {
      *error = L"BGRA texture pool initialization failed: " +
               initialize_error;
    }
    return false;
  }

  auto first = pool.Acquire();
  auto second = pool.Acquire();
  auto exhausted = pool.Acquire();
  if (!first.Succeeded() || !second.Succeeded() ||
      exhausted.status != BgraTexturePoolAcquireStatus::Exhausted) {
    if (error != nullptr) {
      *error = L"BGRA texture pool capacity behavior is incorrect.";
    }
    return false;
  }

  ID3D11Texture2D* first_texture = first.lease.get();
  D3D11_TEXTURE2D_DESC desc{};
  first_texture->GetDesc(&desc);
  if (desc.Width != 64 || desc.Height != 48 ||
      desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    if (error != nullptr) {
      *error = L"BGRA texture pool created a texture with the wrong format.";
    }
    return false;
  }

  first.lease.Reset();
  auto reused = pool.Acquire();
  if (!reused.Succeeded() || reused.lease.get() != first_texture) {
    if (error != nullptr) {
      *error = L"BGRA texture pool did not reuse a returned texture.";
    }
    return false;
  }

  second.lease.Reset();
  reused.lease.Reset();
  const auto stats = pool.stats();
  if (stats.capacity != 2 || stats.allocated_texture_count != 2 ||
      stats.available_texture_count != 2 ||
      stats.in_use_texture_count != 0 ||
      stats.peak_in_use_texture_count != 2 || stats.acquire_count != 3 ||
      stats.created_texture_count != 2 || stats.reused_texture_count != 1 ||
      stats.returned_texture_count != 3 || stats.exhausted_count != 1 ||
      std::wstring(olouie::capture::BgraTexturePoolAcquireStatusName(
          BgraTexturePoolAcquireStatus::Exhausted)) != L"exhausted") {
    if (error != nullptr) {
      *error = L"BGRA texture pool lifecycle statistics are incorrect.";
    }
    return false;
  }

  return true;
}

bool VerifyVideoFrameQueue(ID3D11Device* device, std::wstring* error) {
  using olouie::capture::OwnedVideoFrame;
  using olouie::capture::VideoFrameOverflowPolicy;
  using olouie::capture::VideoFrameQueue;
  using olouie::capture::VideoFrameQueueOptions;
  using olouie::capture::VideoFrameQueueOverflowReason;
  using olouie::capture::VideoFrameQueuePushStatus;

  winrt::com_ptr<ID3D11Texture2D> texture;
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }

  VideoFrameQueue invalid_queue(
      VideoFrameQueueOptions{0, VideoFrameOverflowPolicy::DropNewest});
  if (invalid_queue.Push(MakeQueueTestFrame(std::move(texture), 1)).status !=
      VideoFrameQueuePushStatus::InvalidConfig) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue should reject zero capacity.";
    }
    return false;
  }
  if (invalid_queue.stats().rejected_frame_count != 1) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue should count invalid config rejections.";
    }
    return false;
  }

  VideoFrameQueue newest_queue(
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest});
  for (int64_t ticks : {10, 20, 30}) {
    if (!CreateQueueTestTexture(device, &texture, error)) {
      return false;
    }
    const auto push =
        newest_queue.Push(MakeQueueTestFrame(std::move(texture), ticks));
    if ((ticks < 30 && push.status != VideoFrameQueuePushStatus::Queued) ||
        (ticks == 30 &&
         push.status != VideoFrameQueuePushStatus::DroppedNewest)) {
      if (error != nullptr) {
        *error = L"VideoFrameQueue drop-newest policy returned wrong status.";
      }
      return false;
    }
  }
  auto stats = newest_queue.stats();
  if (newest_queue.Size() != 2 || stats.queued_frame_count != 2 ||
      stats.dropped_newest_count != 1 || stats.overflow_event_count != 1 ||
      stats.peak_depth != 2 || stats.current_depth != 2 ||
      stats.oldest_timestamp_ticks != 10 ||
      stats.newest_timestamp_ticks != 20 ||
      stats.current_oldest_frame_age_ticks != 20 ||
      stats.maximum_oldest_frame_age_ticks != 20 ||
      stats.last_overflow_dropped_frame_count != 1 ||
      stats.last_overflow_reason !=
          VideoFrameQueueOverflowReason::NewestRejected) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue drop-newest stats are incorrect.";
    }
    return false;
  }

  OwnedVideoFrame popped;
  if (!newest_queue.TryPop(&popped) || popped.timestamp_ticks != 10 ||
      !newest_queue.TryPop(&popped) || popped.timestamp_ticks != 20 ||
      newest_queue.TryPop(&popped)) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue FIFO pop is incorrect.";
    }
    return false;
  }

  VideoFrameQueue oldest_queue(
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropOldest});
  for (int64_t ticks : {100, 200, 300}) {
    if (!CreateQueueTestTexture(device, &texture, error)) {
      return false;
    }
    const auto push =
        oldest_queue.Push(MakeQueueTestFrame(std::move(texture), ticks));
    if ((ticks < 300 && push.status != VideoFrameQueuePushStatus::Queued) ||
        (ticks == 300 &&
         push.status != VideoFrameQueuePushStatus::DroppedOldestAndQueued)) {
      if (error != nullptr) {
        *error = L"VideoFrameQueue drop-oldest policy returned wrong status.";
      }
      return false;
    }
  }
  auto drained = oldest_queue.Drain();
  stats = oldest_queue.stats();
  if (drained.size() != 2 || drained[0].timestamp_ticks != 200 ||
      drained[1].timestamp_ticks != 300 ||
      stats.dropped_oldest_count != 1 || stats.popped_frame_count != 2) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue drop-oldest drain is incorrect.";
    }
    return false;
  }

  VideoFrameQueue freshest_queue(
      VideoFrameQueueOptions{3, VideoFrameOverflowPolicy::KeepNewest});
  olouie::capture::BgraTexturePool freshest_pool;
  if (!freshest_pool.Initialize(
          device, olouie::capture::BgraTexturePoolConfig{4, 4, 4}, error)) {
    return false;
  }
  for (int64_t ticks : {100, 200, 300}) {
    auto acquired = freshest_pool.Acquire();
    if (!acquired.Succeeded() ||
        !freshest_queue
             .Push(MakePooledQueueTestFrame(std::move(acquired.lease), ticks))
             .Queued()) {
      if (error != nullptr && error->empty()) {
        *error = L"Freshest-frame queue setup failed.";
      }
      return false;
    }
  }
  auto newest = freshest_pool.Acquire();
  if (!newest.Succeeded()) {
    if (error != nullptr) {
      *error = L"Freshest-frame queue could not lease its newest texture.";
    }
    return false;
  }
  const auto recovered = freshest_queue.Push(
      MakePooledQueueTestFrame(std::move(newest.lease), 400));
  stats = freshest_queue.stats();
  if (recovered.status !=
          VideoFrameQueuePushStatus::DroppedBacklogAndQueued ||
      recovered.dropped_frame_count != 3 || freshest_queue.Size() != 1 ||
      stats.current_depth != 1 || stats.peak_depth != 3 ||
      stats.dropped_backlog_count != 3 || stats.overflow_event_count != 1 ||
      stats.backlog_recovery_count != 1 ||
      stats.current_oldest_frame_age_ticks != 0 ||
      stats.maximum_oldest_frame_age_ticks != 300 ||
      stats.last_overflow_dropped_frame_count != 3 ||
      stats.last_overflow_reason !=
          VideoFrameQueueOverflowReason::BacklogDiscarded ||
      !freshest_queue.TryPop(&popped) || popped.timestamp_ticks != 400 ||
      std::wstring(olouie::capture::VideoFrameQueueOverflowReasonName(
          stats.last_overflow_reason)) != L"stale backlog discarded") {
    if (error != nullptr) {
      *error = L"Freshest-frame overload recovery is incorrect.";
    }
    return false;
  }
  auto pool_stats = freshest_pool.stats();
  if (pool_stats.in_use_texture_count != 1 ||
      pool_stats.available_texture_count != 3 ||
      pool_stats.returned_texture_count != 3) {
    if (error != nullptr) {
      *error = L"Freshest-frame recovery did not return stale pool leases.";
    }
    return false;
  }
  popped = {};
  pool_stats = freshest_pool.stats();
  if (pool_stats.in_use_texture_count != 0 ||
      pool_stats.available_texture_count != 4 ||
      pool_stats.returned_texture_count != 4) {
    if (error != nullptr) {
      *error = L"Popped freshest frame did not return its texture lease.";
    }
    return false;
  }

  VideoFrameQueue invalid_frame_queue(
      VideoFrameQueueOptions{1, VideoFrameOverflowPolicy::DropNewest});
  if (invalid_frame_queue.Push(OwnedVideoFrame{}).status !=
      VideoFrameQueuePushStatus::InvalidFrame) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue should reject invalid frames.";
    }
    return false;
  }

  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  if (!invalid_frame_queue.Push(MakeQueueTestFrame(std::move(texture), 400))
           .Queued()) {
    if (error != nullptr) {
      *error = L"VideoFrameQueue should queue a valid frame after rejection.";
    }
    return false;
  }
  invalid_frame_queue.Clear();
  stats = invalid_frame_queue.stats();
  if (!invalid_frame_queue.Empty() || stats.cleared_frame_count != 1 ||
      std::wstring(olouie::capture::VideoFrameQueuePushStatusName(
          VideoFrameQueuePushStatus::DroppedNewest)) != L"dropped newest") {
    if (error != nullptr) {
      *error = L"VideoFrameQueue clear or status names are incorrect.";
    }
    return false;
  }

  return true;
}

bool VerifyCapturedVideoFrameSink(ID3D11Device* device, std::wstring* error) {
  using olouie::capture::CapturedVideoFrameSinkStatus;
  using olouie::capture::DispatchCapturedVideoFrame;

  if (DispatchCapturedVideoFrame(nullptr, olouie::capture::OwnedVideoFrame{})
          .status != CapturedVideoFrameSinkStatus::InvalidFrame) {
    if (error != nullptr) {
      *error = L"Captured video frame dispatch should reject invalid frames.";
    }
    return false;
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  auto result =
      DispatchCapturedVideoFrame(nullptr,
                                 MakeQueueTestFrame(std::move(texture), 100));
  if (!result.Succeeded() || !result.Accepted() || result.Dropped()) {
    if (error != nullptr) {
      *error = L"Captured video frame dispatch should allow null sinks.";
    }
    return false;
  }

  FakeCapturedVideoFrameSink sink;
  sink.result = CapturedVideoFrameResult(
      CapturedVideoFrameSinkStatus::DroppedAndQueued);
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  result = DispatchCapturedVideoFrame(&sink,
                                      MakeQueueTestFrame(std::move(texture),
                                                         200));
  if (!result.Succeeded() || !result.Accepted() || !result.Dropped() ||
      sink.received_frame_count != 1 ||
      sink.last_frame.timestamp_ticks != 200 ||
      std::wstring(olouie::capture::CapturedVideoFrameSinkStatusName(
          CapturedVideoFrameSinkStatus::DroppedAndQueued)) !=
          L"dropped and queued") {
    if (error != nullptr) {
      *error = L"Captured video frame sink result helpers are incorrect.";
    }
    return false;
  }

  return true;
}

bool VerifyVideoEncodeWorker(ID3D11Device* device, std::wstring* error) {
  using olouie::capture::VideoFrameOverflowPolicy;
  using olouie::capture::VideoFrameQueue;
  using olouie::capture::VideoFrameQueueOptions;
  using olouie::capture::VideoFrameQueuePushStatus;
  using olouie::encode::BgraVideoRecordingSession;
  using olouie::encode::BgraVideoRecordingSessionOptions;
  using olouie::encode::VideoEncodeWorker;
  using olouie::encode::VideoEncodeWorkerOptions;
  using olouie::encode::VideoEncodeWorkerStatus;

  std::wstring timebase_error;
  const auto timebase =
      olouie::record::Timebase::FromQpc(10000000, 100, &timebase_error);
  if (!timebase.IsValid()) {
    if (error != nullptr) {
      *error = timebase_error;
    }
    return false;
  }

  constexpr int64_t kFallbackDurationNs = 16666667;
  BgraVideoRecordingSession session(
      BgraVideoRecordingSessionOptions{1, 3000, 4, 4});
  VideoFrameQueue empty_queue(
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest});

  VideoEncodeWorker invalid_worker(
      nullptr, &session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  if (invalid_worker.DrainQueuedFrames(1).status !=
      VideoEncodeWorkerStatus::InvalidArgument) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should reject a missing queue.";
    }
    return false;
  }

  VideoEncodeWorker invalid_options_worker(
      &empty_queue, &session, VideoEncodeWorkerOptions{timebase, 0});
  if (invalid_options_worker.DrainQueuedFrames(1).status !=
      VideoEncodeWorkerStatus::InvalidArgument) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should reject invalid timing options.";
    }
    return false;
  }

  VideoEncodeWorker empty_worker(
      &empty_queue, &session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  auto result = empty_worker.DrainQueuedFrames(1);
  if (!result.Succeeded() || result.popped_frame_count != 0 ||
      result.processed_frame_count != 0 ||
      result.remaining_frame_count != 0) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should allow an empty queue.";
    }
    return false;
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  VideoFrameQueue budget_queue(
      VideoFrameQueueOptions{1, VideoFrameOverflowPolicy::DropNewest});
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  if (!budget_queue.Push(MakeQueueTestFrame(std::move(texture), 100))
           .Queued()) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker budget test frame did not queue.";
    }
    return false;
  }

  VideoEncodeWorker budget_worker(
      &budget_queue, &session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  result = budget_worker.DrainQueuedFrames(0);
  if (!result.Succeeded() || result.popped_frame_count != 0 ||
      result.processed_frame_count != 0 ||
      result.remaining_frame_count != 1 || budget_queue.Size() != 1) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should honor a zero frame budget.";
    }
    return false;
  }
  budget_queue.Clear();

  VideoFrameQueue timing_queue(
      VideoFrameQueueOptions{1, VideoFrameOverflowPolicy::DropNewest});
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  if (!timing_queue.Push(MakeQueueTestFrame(std::move(texture), 99))
           .Queued()) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker timing test frame did not queue.";
    }
    return false;
  }

  VideoEncodeWorker timing_worker(
      &timing_queue, &session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  result = timing_worker.DrainQueuedFrames(2);
  if (result.status != VideoEncodeWorkerStatus::FrameTimingError ||
      result.popped_frame_count != 1 || result.failed_frame_count != 1 ||
      result.remaining_frame_count != 0 ||
      timing_worker.stats().timing_error_count != 1) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should reject pre-session timestamps.";
    }
    return false;
  }

  BgraVideoRecordingSession mismatch_session(
      BgraVideoRecordingSessionOptions{1, 3000, 8, 4});
  VideoFrameQueue mismatch_queue(
      VideoFrameQueueOptions{1, VideoFrameOverflowPolicy::DropNewest});
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }
  if (!mismatch_queue.Push(MakeQueueTestFrame(std::move(texture), 100))
           .Queued()) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker mismatch test frame did not queue.";
    }
    return false;
  }

  VideoEncodeWorker mismatch_worker(
      &mismatch_queue, &mismatch_session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  result = mismatch_worker.DrainQueuedFrames(1);
  if (result.status != VideoEncodeWorkerStatus::FrameFormatMismatch ||
      result.popped_frame_count != 1 || result.failed_frame_count != 1 ||
      mismatch_worker.stats().format_mismatch_count != 1) {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker should reject mismatched frame sizes.";
    }
    return false;
  }

  VideoFrameQueue failure_queue(
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest});
  for (int64_t ticks : {100, 110, 120}) {
    if (!CreateQueueTestTexture(device, &texture, error)) {
      return false;
    }
    const auto push =
        failure_queue.Push(MakeQueueTestFrame(std::move(texture), ticks));
    if ((ticks < 120 && push.status != VideoFrameQueuePushStatus::Queued) ||
        (ticks == 120 &&
         push.status != VideoFrameQueuePushStatus::DroppedNewest)) {
      if (error != nullptr) {
        *error = L"VideoEncodeWorker failure queue setup dropped wrongly.";
      }
      return false;
    }
  }

  VideoEncodeWorker failure_worker(
      &failure_queue, &session,
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs});
  result = failure_worker.DrainQueuedFrames(3);
  if (result.status != VideoEncodeWorkerStatus::SessionError ||
      result.first_failure.status !=
          olouie::encode::VideoRecordingSessionStatus::InvalidState ||
      result.popped_frame_count != 1 || result.processed_frame_count != 0 ||
      result.failed_frame_count != 1 || result.remaining_frame_count != 1 ||
      result.dropped_frame_count != 1 ||
      failure_worker.stats().session_error_count != 1) {
    if (error != nullptr) {
      *error =
          L"VideoEncodeWorker should preserve first session failure status.";
    }
    return false;
  }

  if (std::wstring(olouie::encode::VideoEncodeWorkerStatusName(
          VideoEncodeWorkerStatus::SessionError)) != L"session error") {
    if (error != nullptr) {
      *error = L"VideoEncodeWorker status names changed unexpectedly.";
    }
    return false;
  }

  return true;
}

bool VerifyVideoEncodeChain(ID3D11Device* device, std::wstring* error) {
  using olouie::capture::OwnedVideoFrame;
  using olouie::capture::VideoFrameOverflowPolicy;
  using olouie::capture::VideoFrameQueueOptions;
  using olouie::capture::VideoFrameQueuePushStatus;
  using olouie::encode::BgraVideoRecordingSessionOptions;
  using olouie::encode::VideoEncodeChain;
  using olouie::encode::VideoEncodeChainConfig;
  using olouie::encode::VideoEncodeWorkerOptions;
  using olouie::encode::VideoEncodeWorkerStatus;
  using olouie::encode::VideoRecordingSessionStatus;

  std::wstring timebase_error;
  const auto timebase =
      olouie::record::Timebase::FromQpc(10000000, 100, &timebase_error);
  if (!timebase.IsValid()) {
    if (error != nullptr) {
      *error = timebase_error;
    }
    return false;
  }

  constexpr int64_t kFallbackDurationNs = 16666667;

  VideoEncodeChain invalid_chain(VideoEncodeChainConfig{});
  auto prepare = invalid_chain.Prepare(nullptr, nullptr, nullptr, nullptr);
  if (invalid_chain.IsConfigured() ||
      prepare.status != VideoRecordingSessionStatus::InvalidConfig ||
      invalid_chain.IsPrepared()) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain should reject invalid configuration.";
    }
    return false;
  }

  VideoEncodeChainConfig config{
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest},
      BgraVideoRecordingSessionOptions{1, 3000, 4, 4},
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs},
      1};
  if (!config.IsValid()) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain test configuration should be valid.";
    }
    return false;
  }

  VideoEncodeChain chain(config);
  if (!chain.IsConfigured() || chain.IsPrepared() ||
      chain.queued_frame_count() != 0 || !chain.queue_empty()) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain initial state is incorrect.";
    }
    return false;
  }

  prepare = chain.Prepare(nullptr, nullptr, nullptr, nullptr);
  if (prepare.status != VideoRecordingSessionStatus::InvalidState ||
      chain.prepare_result().status != VideoRecordingSessionStatus::InvalidState ||
      chain.IsPrepared()) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain should surface BGRA session prepare failure.";
    }
    return false;
  }

  if (chain.QueueFrame(OwnedVideoFrame{}).status !=
      VideoFrameQueuePushStatus::InvalidFrame) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain should reject invalid queued frames.";
    }
    return false;
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  for (int64_t ticks : {100, 110}) {
    if (!CreateQueueTestTexture(device, &texture, error)) {
      return false;
    }
    if (!chain.QueueFrame(MakeQueueTestFrame(std::move(texture), ticks))
             .Queued()) {
      if (error != nullptr) {
        *error = L"VideoEncodeChain should queue valid frames.";
      }
      return false;
    }
  }

  if (chain.queued_frame_count() != 2 ||
      chain.queue_stats().queued_frame_count != 2) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain queue ownership stats are incorrect.";
    }
    return false;
  }

  auto drain = chain.DrainQueuedFrames(0);
  if (!drain.Succeeded() || drain.popped_frame_count != 0 ||
      drain.remaining_frame_count != 2 || chain.queued_frame_count() != 2) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain should honor explicit zero drain budget.";
    }
    return false;
  }

  drain = chain.DrainQueuedFrames();
  if (drain.status != VideoEncodeWorkerStatus::SessionError ||
      drain.popped_frame_count != 1 || drain.remaining_frame_count != 1 ||
      drain.first_failure.status != VideoRecordingSessionStatus::InvalidState ||
      chain.worker_stats().session_error_count != 1 ||
      chain.session_stats().submitted_frame_count != 0) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain should use its configured drain budget.";
    }
    return false;
  }

  chain.ClearQueuedFrames();
  if (!chain.queue_empty() || chain.queue_stats().cleared_frame_count != 1) {
    if (error != nullptr) {
      *error = L"VideoEncodeChain clear behavior is incorrect.";
    }
    return false;
  }

  return true;
}

bool VerifyVideoCaptureEncodeBridge(ID3D11Device* device,
                                    std::wstring* error) {
  using olouie::capture::CapturedVideoFrameSinkStatus;
  using olouie::capture::VideoFrameOverflowPolicy;
  using olouie::capture::VideoFrameQueueOptions;
  using olouie::encode::BgraVideoRecordingSessionOptions;
  using olouie::encode::VideoCaptureEncodeBridge;
  using olouie::encode::VideoCaptureEncodeBridgeOptions;
  using olouie::encode::VideoEncodeChain;
  using olouie::encode::VideoEncodeChainConfig;
  using olouie::encode::VideoEncodeWorkerOptions;
  using olouie::encode::VideoEncodeWorkerStatus;
  using olouie::encode::VideoRecordingSessionStatus;

  std::wstring timebase_error;
  const auto timebase =
      olouie::record::Timebase::FromQpc(10000000, 0, &timebase_error);
  if (!timebase.IsValid()) {
    if (error != nullptr) {
      *error = timebase_error;
    }
    return false;
  }

  constexpr int64_t kFallbackDurationNs = 16666667;
  VideoEncodeChainConfig config{
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest},
      BgraVideoRecordingSessionOptions{1, 3000, 4, 4},
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs},
      1};
  VideoEncodeChain chain(config);
  VideoCaptureEncodeBridge bridge(
      &chain, VideoCaptureEncodeBridgeOptions{10000000, true});

  if (!bridge.IsConfigured() || bridge.captured_frame_sink() != &bridge) {
    if (error != nullptr) {
      *error = L"VideoCaptureEncodeBridge should expose a configured sink.";
    }
    return false;
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  if (!CreateQueueTestTexture(device, &texture, error)) {
    return false;
  }

  VideoCaptureEncodeBridge null_bridge(
      nullptr, VideoCaptureEncodeBridgeOptions{10000000, true});
  auto delivered =
      null_bridge.OnCapturedVideoFrame(MakeQueueTestFrame(std::move(texture),
                                                          1000));
  if (delivered.status != CapturedVideoFrameSinkStatus::InvalidConfig) {
    if (error != nullptr) {
      *error = L"VideoCaptureEncodeBridge should reject missing chains.";
    }
    return false;
  }

  delivered =
      bridge.OnCapturedVideoFrame(olouie::capture::OwnedVideoFrame{});
  if (delivered.status != CapturedVideoFrameSinkStatus::InvalidFrame ||
      bridge.stats().invalid_frame_count != 1) {
    if (error != nullptr) {
      *error = L"VideoCaptureEncodeBridge should reject invalid frames.";
    }
    return false;
  }

  for (int64_t ticks : {1000, 1010, 1020}) {
    if (!CreateQueueTestTexture(device, &texture, error)) {
      return false;
    }
    delivered = bridge.OnCapturedVideoFrame(
        MakeQueueTestFrame(std::move(texture), ticks));
    if (ticks < 1020 &&
        delivered.status != CapturedVideoFrameSinkStatus::Success) {
      if (error != nullptr) {
        *error = L"VideoCaptureEncodeBridge should queue early frames.";
      }
      return false;
    }
    if (ticks == 1020 &&
        (delivered.status != CapturedVideoFrameSinkStatus::Dropped ||
         delivered.dropped_frame_count != 1)) {
      if (error != nullptr) {
        *error = L"VideoCaptureEncodeBridge should surface dropped frames.";
      }
      return false;
    }
  }

  if (bridge.first_timestamp_ticks() != 1000 ||
      bridge.stats().timebase_start_count != 1 ||
      bridge.stats().queued_frame_count != 2 ||
      bridge.stats().dropped_frame_count != 1 ||
      chain.config().worker_options.timebase.session_start_qpc() != 1000 ||
      chain.queued_frame_count() != 2) {
    if (error != nullptr) {
      *error =
          L"VideoCaptureEncodeBridge first-frame timebase or stats are wrong.";
    }
    return false;
  }

  auto drain = bridge.DrainQueuedFrames(0);
  if (!drain.Succeeded() || drain.remaining_frame_count != 2) {
    if (error != nullptr) {
      *error = L"VideoCaptureEncodeBridge should honor explicit drain budget.";
    }
    return false;
  }

  drain = bridge.DrainQueuedFrames();
  if (drain.status != VideoEncodeWorkerStatus::SessionError ||
      drain.first_failure.status != VideoRecordingSessionStatus::InvalidState ||
      drain.popped_frame_count != 1 || drain.remaining_frame_count != 1) {
    if (error != nullptr) {
      *error =
          L"VideoCaptureEncodeBridge should drain through the video chain.";
    }
    return false;
  }

  return true;
}

bool VerifyVideoLiveCaptureEncode(ID3D11Device* device,
                                  ID3D11DeviceContext* context,
                                  std::wstring* error) {
  using olouie::capture::VideoFrameOverflowPolicy;
  using olouie::capture::VideoFrameQueueOptions;
  using olouie::encode::BgraVideoRecordingSessionOptions;
  using olouie::encode::RunWgcVideoLiveCaptureEncode;
  using olouie::encode::VideoEncodeChain;
  using olouie::encode::VideoEncodeChainConfig;
  using olouie::encode::VideoEncodeWorkerOptions;
  using olouie::encode::VideoLiveCaptureEncodeOptions;
  using olouie::encode::VideoLiveCaptureEncodeResult;
  using olouie::encode::VideoLiveCaptureEncodeStatus;
  using olouie::encode::VideoRecordingSessionStatus;

  std::wstring timebase_error;
  const auto timebase =
      olouie::record::Timebase::FromQpc(10000000, 0, &timebase_error);
  if (!timebase.IsValid()) {
    if (error != nullptr) {
      *error = timebase_error;
    }
    return false;
  }

  constexpr int64_t kFallbackDurationNs = 16666667;
  const VideoEncodeChainConfig config{
      VideoFrameQueueOptions{2, VideoFrameOverflowPolicy::DropNewest},
      BgraVideoRecordingSessionOptions{1, 3000, 4, 4},
      VideoEncodeWorkerOptions{timebase, kFallbackDurationNs},
      1};
  const VideoLiveCaptureEncodeOptions options{
      std::chrono::milliseconds(20),
      std::chrono::milliseconds(1),
      1,
      1,
      10000000,
      true};

  VideoEncodeChain invalid_options_chain(config);
  VideoLiveCaptureEncodeResult live_result;
  auto run = RunWgcVideoLiveCaptureEncode(
      reinterpret_cast<HMONITOR>(1), device, context,
      VideoLiveCaptureEncodeOptions{}, &invalid_options_chain,
      &FakeVideoFrameCopyRunner, &live_result);
  if (run.status != VideoLiveCaptureEncodeStatus::InvalidConfig) {
    if (error != nullptr) {
      *error = L"Video live capture encode should reject invalid options.";
    }
    return false;
  }

  VideoEncodeChain no_frame_chain(config);
  g_fake_video_frame_copy_mode = FakeVideoFrameCopyMode::NoFrames;
  run = RunWgcVideoLiveCaptureEncode(
      reinterpret_cast<HMONITOR>(1), device, context, options, &no_frame_chain,
      &FakeVideoFrameCopyRunner, &live_result);
  if (!run.Succeeded() || live_result.capture.copied_frame_count != 0 ||
      !live_result.final_drain.Succeeded() ||
      live_result.bridge_stats.received_frame_count != 0) {
    if (error != nullptr) {
      *error = L"Video live capture encode should succeed with no fake frames.";
    }
    return false;
  }

  VideoEncodeChain capture_error_chain(config);
  g_fake_video_frame_copy_mode = FakeVideoFrameCopyMode::CaptureError;
  run = RunWgcVideoLiveCaptureEncode(
      reinterpret_cast<HMONITOR>(1), device, context, options,
      &capture_error_chain, &FakeVideoFrameCopyRunner, &live_result);
  if (run.status != VideoLiveCaptureEncodeStatus::CaptureFailed ||
      live_result.capture.error != L"fake capture failure") {
    if (error != nullptr) {
      *error = L"Video live capture encode should surface capture failures.";
    }
    return false;
  }

  VideoEncodeChain one_frame_chain(config);
  g_fake_video_frame_copy_mode = FakeVideoFrameCopyMode::OneFrame;
  run = RunWgcVideoLiveCaptureEncode(
      reinterpret_cast<HMONITOR>(1), device, context, options,
      &one_frame_chain, &FakeVideoFrameCopyRunner, &live_result);
  const auto& failing_drain = live_result.final_drain.Succeeded()
                                  ? live_result.last_tick_drain
                                  : live_result.final_drain;
  if (run.status != VideoLiveCaptureEncodeStatus::DrainFailed ||
      live_result.capture.copied_frame_count != 1 ||
      live_result.bridge_stats.queued_frame_count != 1 ||
      live_result.first_timestamp_ticks != 1000 ||
      one_frame_chain.config().worker_options.timebase.session_start_qpc() !=
          1000 ||
      failing_drain.first_failure.status !=
          VideoRecordingSessionStatus::InvalidState ||
      failing_drain.popped_frame_count != 1 ||
      std::wstring(olouie::encode::VideoLiveCaptureEncodeStatusName(
          VideoLiveCaptureEncodeStatus::DrainFailed)) != L"drain failed") {
    if (error != nullptr) {
      *error =
          L"Video live capture encode should drain fake frames through the chain.";
    }
    return false;
  }

  return true;
}

bool VerifyWgcMonitorCaptureFaultLatch(std::wstring* error) {
  using olouie::capture::WgcMonitorCaptureFault;
  using olouie::capture::WgcMonitorCaptureFaultKind;
  using olouie::capture::WgcMonitorCaptureFaultLatch;

  WgcMonitorCaptureFaultLatch latch;
  if (latch.Snapshot().Failed() ||
      latch.Report(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::None, L"not a fault"}) ||
      latch.Report(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::MonitorResized, L""})) {
    if (error != nullptr) {
      *error = L"The WGC fault latch accepted an invalid fault.";
    }
    return false;
  }

  const WgcMonitorCaptureFault resize{
      WgcMonitorCaptureFaultKind::MonitorResized,
      L"selected monitor resized",
      1920,
      1080,
      1280,
      720};
  if (!latch.Report(resize) ||
      latch.Report(WgcMonitorCaptureFault{
          WgcMonitorCaptureFaultKind::MonitorDisconnected,
          L"selected monitor disconnected",
          1920,
          1080})) {
    if (error != nullptr) {
      *error = L"The WGC fault latch did not preserve the first fault.";
    }
    return false;
  }

  const auto first = latch.Snapshot();
  if (first.kind != WgcMonitorCaptureFaultKind::MonitorResized ||
      first.message != resize.message || first.expected_width != 1920 ||
      first.expected_height != 1080 || first.observed_width != 1280 ||
      first.observed_height != 720 ||
      std::wstring(olouie::capture::WgcMonitorCaptureFaultKindName(
          first.kind)) != L"monitor resized" ||
      !olouie::capture::IsSelectedMonitorTopologyFault(first.kind) ||
      olouie::capture::IsSelectedMonitorTopologyFault(
          WgcMonitorCaptureFaultKind::CallbackFailed)) {
    if (error != nullptr) {
      *error = L"The WGC fault latch changed the first resize details.";
    }
    return false;
  }

  latch.Reset();
  const WgcMonitorCaptureFault disconnected{
      WgcMonitorCaptureFaultKind::MonitorDisconnected,
      L"selected monitor disconnected",
      1920,
      1080};
  if (!latch.Report(disconnected) ||
      latch.Snapshot().kind !=
          WgcMonitorCaptureFaultKind::MonitorDisconnected ||
      std::wstring(olouie::capture::WgcMonitorCaptureFaultKindName(
          latch.Snapshot().kind)) != L"monitor disconnected" ||
      !olouie::capture::IsSelectedMonitorTopologyFault(
          latch.Snapshot().kind)) {
    if (error != nullptr) {
      *error = L"The WGC fault latch did not reset for a disconnect.";
    }
    return false;
  }

  latch.Reset();
  WgcMonitorCaptureFault device_lost;
  device_lost.kind = WgcMonitorCaptureFaultKind::D3D11DeviceLost;
  device_lost.message = L"D3D11 device reset during frame copy";
  device_lost.device_fault =
      olouie::graphics::ClassifyD3D11DeviceFault(
          DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
          L"WGC frame copy");
  if (!latch.Report(device_lost) ||
      !latch.Snapshot().device_fault.Failed() ||
      latch.Snapshot().device_fault.kind !=
          olouie::graphics::D3D11DeviceFaultKind::Removed ||
      !olouie::capture::IsWgcD3D11DeviceFault(
          latch.Snapshot().kind) ||
      olouie::capture::IsSelectedMonitorTopologyFault(
          latch.Snapshot().kind) ||
      std::wstring(olouie::capture::WgcMonitorCaptureFaultKindName(
          latch.Snapshot().kind)) != L"D3D11 device lost") {
    if (error != nullptr) {
      *error = L"The WGC fault latch did not preserve typed D3D11 loss.";
    }
    return false;
  }
  return true;
}

bool VerifyD3D11DeviceFaultClassification(std::wstring* error) {
  using olouie::graphics::ClassifyD3D11DeviceFault;
  using olouie::graphics::D3D11DeviceFaultKind;

  const auto healthy =
      ClassifyD3D11DeviceFault(S_OK, S_OK, L"healthy operation");
  const auto removed = ClassifyD3D11DeviceFault(
      DXGI_ERROR_DEVICE_REMOVED, S_OK, L"texture copy");
  const auto reset = ClassifyD3D11DeviceFault(
      E_FAIL, DXGI_ERROR_DEVICE_RESET, L"video processor blit");
  const auto hung = ClassifyD3D11DeviceFault(
      S_OK, DXGI_ERROR_DEVICE_HUNG, L"encoder input");
  const auto driver = ClassifyD3D11DeviceFault(
      S_OK, DXGI_ERROR_DRIVER_INTERNAL_ERROR, L"encoder drain");
  const auto unknown =
      ClassifyD3D11DeviceFault(S_OK, E_FAIL, L"device poll");
  if (healthy.Failed() ||
      removed.kind != D3D11DeviceFaultKind::Removed ||
      reset.kind != D3D11DeviceFaultKind::Reset ||
      hung.kind != D3D11DeviceFaultKind::Hung ||
      driver.kind != D3D11DeviceFaultKind::DriverInternalError ||
      unknown.kind != D3D11DeviceFaultKind::Unknown ||
      removed.message.find(L"texture copy") == std::wstring::npos ||
      reset.message.find(L"0x887A0007") == std::wstring::npos ||
      !olouie::graphics::IsD3D11DeviceLossResult(
          DXGI_ERROR_DEVICE_HUNG) ||
      olouie::graphics::IsD3D11DeviceLossResult(E_FAIL) ||
      std::wstring(olouie::graphics::D3D11DeviceFaultKindName(
          D3D11DeviceFaultKind::DriverInternalError)) !=
          L"driver internal error") {
    if (error != nullptr) {
      *error = L"D3D11 removed/reset/hung classification is incorrect.";
    }
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() / L"O'LouieSmokeTestRuntime";
  const auto paths = olouie::logging::RuntimePaths::FromRoot(root);

  if (paths.settings.filename() != L"settings" ||
      paths.logs.filename() != L"logs" ||
      paths.sessions.filename() != L"sessions" ||
      paths.exports.filename() != L"exports" ||
      paths.cache.filename() != L"cache") {
    return Fail("Runtime path layout is not stable.");
  }

  const auto settings = olouie::settings::AppSettings::Defaults(paths);
  std::wstring validation_error;
  if (!olouie::settings::Validate(settings, &validation_error)) {
    std::wcerr << L"Default settings failed validation: " << validation_error
               << L'\n';
    return 1;
  }
  if (settings.clips.presets_seconds != std::vector<int>({30, 60, 300}) ||
      settings.clips.bookmark_pre_seconds != 60 ||
      settings.clips.bookmark_post_seconds != 0 ||
      settings.hotkeys.save_last_5m != L"Ctrl+Shift+F9" ||
      settings.hotkeys.bookmark != L"Ctrl+Shift+F10") {
    return Fail("Default clip preset/bookmark command settings changed.");
  }
  auto invalid_bookmark_settings = settings;
  invalid_bookmark_settings.clips.bookmark_pre_seconds = 0;
  invalid_bookmark_settings.clips.bookmark_post_seconds = 0;
  if (olouie::settings::Validate(invalid_bookmark_settings,
                                  &validation_error)) {
    return Fail("Settings should reject an empty bookmark export window.");
  }

  std::wstring settings_test_error;
  if (!VerifySettingsPersistence(paths, &settings_test_error)) {
    std::wcerr << L"Settings persistence tests failed: "
               << settings_test_error << L'\n';
    return 1;
  }
  if (!VerifyRuntimePathMigration(&settings_test_error)) {
    std::wcerr << L"Runtime path migration tests failed: "
               << settings_test_error << L'\n';
    return 1;
  }
  if (!VerifyHotkeyParsingAndTransactions(&settings_test_error)) {
    std::wcerr << L"Hotkey settings tests failed: " << settings_test_error
               << L'\n';
    return 1;
  }
  if (!VerifyImGuiHostLifecycle(&settings_test_error)) {
    std::wcerr << L"Settings UI lifecycle tests failed: "
               << settings_test_error << L'\n';
    return 1;
  }

  const auto presets = olouie::settings::DefaultVideoPresets();
  if (presets.size() != 3) {
    return Fail("Expected three default video presets.");
  }

  if (presets[1].name != L"Balanced" || presets[1].fps != 60 ||
      presets[1].gop_seconds != 2.0) {
    return Fail("Balanced preset defaults changed unexpectedly.");
  }

  const auto monitors = olouie::graphics::EnumerateMonitors();
  if (monitors.empty()) {
    return Fail("Expected at least one monitor for the Windows recorder shell.");
  }

  const auto* primary = olouie::graphics::FindPrimaryMonitor(monitors);
  if (primary == nullptr || primary->Width() <= 0 || primary->Height() <= 0) {
    return Fail("Primary monitor metadata is invalid.");
  }

  std::wstring d3d_error;
  auto d3d =
      olouie::graphics::D3D11DeviceContext::CreateForMonitor(primary->handle,
                                                             &d3d_error);
  if (!d3d.IsValid()) {
    std::wcerr << L"D3D11 smoke failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyBgraTexturePool(d3d.device(), &d3d_error)) {
    std::wcerr << L"BGRA texture pool smoke failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyVideoFrameQueue(d3d.device(), &d3d_error)) {
    std::wcerr << L"Video frame queue smoke failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyVideoFrameCadence(&d3d_error)) {
    std::wcerr << L"Video frame cadence tests failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyCapturedVideoFrameSink(d3d.device(), &d3d_error)) {
    std::wcerr << L"Captured video frame sink smoke failed: " << d3d_error
               << L'\n';
    return 1;
  }

  if (!VerifyVideoEncodeWorker(d3d.device(), &d3d_error)) {
    std::wcerr << L"Video encode worker smoke failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyVideoEncodeChain(d3d.device(), &d3d_error)) {
    std::wcerr << L"Video encode chain smoke failed: " << d3d_error << L'\n';
    return 1;
  }

  if (!VerifyVideoCaptureEncodeBridge(d3d.device(), &d3d_error)) {
    std::wcerr << L"Video capture encode bridge smoke failed: " << d3d_error
               << L'\n';
    return 1;
  }

  if (!VerifyVideoLiveCaptureEncode(d3d.device(), d3d.immediate_context(),
                                    &d3d_error)) {
    std::wcerr << L"Video live capture encode smoke failed: " << d3d_error
               << L'\n';
    return 1;
  }

  if (!VerifyWgcMonitorCaptureFaultLatch(&d3d_error)) {
    std::wcerr << L"WGC monitor-fault latch smoke failed: " << d3d_error
               << L'\n';
    return 1;
  }

  if (!VerifyD3D11DeviceFaultClassification(&d3d_error)) {
    std::wcerr << L"D3D11 device-fault classification failed: " << d3d_error
               << L'\n';
    return 1;
  }

  olouie::graphics::GpuBgraToNv12Plan convert_plan;
  if (!olouie::graphics::BuildGpuBgraToNv12Plan(
          {1920, 1080, 1920, 1080}, &convert_plan, &d3d_error) ||
      !convert_plan.IsValid() ||
      convert_plan.source_format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      convert_plan.output_format != DXGI_FORMAT_NV12) {
    std::wcerr << L"BGRA-to-NV12 plan failed: " << d3d_error << L'\n';
    return 1;
  }

  if (olouie::graphics::BuildGpuBgraToNv12Plan(
          {1920, 1080, 1919, 1080}, &convert_plan, &d3d_error)) {
    return Fail("BGRA-to-NV12 plan should reject odd NV12 width.");
  }

  const auto convert_smoke =
      olouie::graphics::CreateGpuBgraToNv12SmokeTextures(
          d3d.device(), {static_cast<uint32_t>(primary->Width()),
                         static_cast<uint32_t>(primary->Height()),
                         static_cast<uint32_t>(primary->Width() & ~1),
                         static_cast<uint32_t>(primary->Height() & ~1)});
  if (!convert_smoke.Succeeded() || !convert_smoke.source_texture_created ||
      !convert_smoke.output_texture_created || !convert_smoke.plan.IsValid()) {
    std::wcerr << L"BGRA-to-NV12 texture smoke failed: "
               << convert_smoke.message << L'\n';
    return 1;
  }

  const auto convert_processor_smoke =
      olouie::graphics::RunGpuBgraToNv12VideoProcessorSmoke(
          d3d.device(), d3d.immediate_context(),
          {static_cast<uint32_t>(primary->Width()),
           static_cast<uint32_t>(primary->Height()),
           static_cast<uint32_t>(primary->Width() & ~1),
           static_cast<uint32_t>(primary->Height() & ~1)});
  if (!convert_processor_smoke.Succeeded() ||
      !convert_processor_smoke.source_texture_created ||
      !convert_processor_smoke.output_texture_created ||
      !convert_processor_smoke.video_processor_created ||
      !convert_processor_smoke.conversion_executed ||
      !convert_processor_smoke.input_view_reused ||
      !convert_processor_smoke.output_view_reused ||
      !convert_processor_smoke.plan.IsValid()) {
    std::wcerr << L"BGRA-to-NV12 VideoProcessor smoke failed: "
               << convert_processor_smoke.message << L'\n';
    return 1;
  }

  olouie::graphics::GpuBgraToNv12Converter converter;
  const auto converter_init = converter.Initialize(
      d3d.device(), d3d.immediate_context(),
      {static_cast<uint32_t>(primary->Width()),
       static_cast<uint32_t>(primary->Height()),
       static_cast<uint32_t>(primary->Width() & ~1),
       static_cast<uint32_t>(primary->Height() & ~1)});
  if (!converter_init.Succeeded() || !converter.IsInitialized() ||
      !converter.plan().IsValid()) {
    std::wcerr << L"BGRA-to-NV12 converter initialization failed: "
               << converter_init.message << L'\n';
    return 1;
  }

  const auto mismatch_result = converter.Convert(nullptr, nullptr);
  if (mismatch_result.status !=
      olouie::graphics::GpuBgraToNv12Status::InvalidConfig) {
    return Fail("BGRA-to-NV12 converter should reject missing textures.");
  }

  if (std::wstring(olouie::graphics::GpuBgraToNv12StatusName(
          olouie::graphics::GpuBgraToNv12Status::ConversionFailed)) !=
      L"conversion failed") {
    return Fail("BGRA-to-NV12 status names changed unexpectedly.");
  }

  std::wstring affinity_error;
  if (!VerifyCaptureExclusionHelper(&affinity_error)) {
    std::wcerr << L"Capture-exclusion smoke failed: " << affinity_error
               << L'\n';
    return 1;
  }

  return 0;
}
