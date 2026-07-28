#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "logging/RuntimePaths.h"
#include "performance/CapturePerformance.h"

namespace olouie::settings {

enum class ResolutionMode {
  Source,
  Custom,
};

struct VideoSettings {
  std::wstring monitor_device_name;
  ResolutionMode resolution_mode = ResolutionMode::Source;
  int custom_width = 1920;
  int custom_height = 1080;
  int fps = 60;
  int bitrate_mbps = 20;
  double gop_seconds = 2.0;
  bool capture_cursor = true;
  performance::CapturePerformanceMode performance_mode =
      performance::CapturePerformanceMode::Balanced;
  std::wstring encoder_backend = L"media_foundation_hardware";
};

struct AudioSettings {
  bool system_mix = true;
  bool mic = false;
  bool separate_tracks = true;
  bool default_mixed_track = true;
  int sample_rate = 48000;
  std::wstring mic_check_output_device_id;
};

struct ClipSettings {
  std::vector<int> presets_seconds = {30, 60, 300};
  int custom_seconds = 120;
  int bookmark_pre_seconds = 60;
  int bookmark_post_seconds = 0;
};

struct HotkeySettings {
  std::wstring toggle_recording = L"Ctrl+Shift+R";
  std::wstring save_last_30s = L"Ctrl+Shift+F8";
  std::wstring save_last_5m = L"Ctrl+Shift+F9";
  std::wstring bookmark = L"Ctrl+Shift+F10";
};

struct VideoPreset {
  std::wstring name;
  ResolutionMode resolution_mode = ResolutionMode::Source;
  int custom_width = 1920;
  int custom_height = 1080;
  int fps = 60;
  int bitrate_mbps = 20;
  double gop_seconds = 2.0;
};

struct AppSettings {
  std::filesystem::path output_directory;
  bool start_with_windows = false;
  bool show_overlay_notifications = true;
  VideoSettings video;
  AudioSettings audio;
  ClipSettings clips;
  HotkeySettings hotkeys;

  static AppSettings Defaults(const logging::RuntimePaths& paths);
};

std::vector<VideoPreset> DefaultVideoPresets();
bool Validate(const AppSettings& settings, std::wstring* error);

}  // namespace olouie::settings
