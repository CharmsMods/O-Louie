#include "settings/Settings.h"

#include <set>
#include <string>

#include "settings/HotkeyBinding.h"

namespace olouie::settings {
namespace {

bool IsEvenPositive(int value) {
  return value > 0 && (value % 2) == 0;
}

bool Fail(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

}  // namespace

AppSettings AppSettings::Defaults(const logging::RuntimePaths& paths) {
  AppSettings settings;
  settings.output_directory = paths.exports;
  return settings;
}

std::vector<VideoPreset> DefaultVideoPresets() {
  return {
      {L"Low", ResolutionMode::Custom, 1920, 1080, 30, 10, 2.0},
      {L"Balanced", ResolutionMode::Source, 1920, 1080, 60, 20, 2.0},
      {L"High", ResolutionMode::Source, 1920, 1080, 60, 35, 2.0},
  };
}

bool Validate(const AppSettings& settings, std::wstring* error) {
  if (settings.output_directory.empty()) {
    return Fail(error, L"Output directory must not be empty.");
  }

  if (!IsEvenPositive(settings.video.custom_width) ||
      !IsEvenPositive(settings.video.custom_height)) {
    return Fail(error, L"Custom video dimensions must be positive even values.");
  }

  if (settings.video.fps <= 0 || settings.video.fps > 240) {
    return Fail(error, L"Video FPS must be between 1 and 240.");
  }

  if (settings.video.bitrate_mbps <= 0 ||
      settings.video.bitrate_mbps > 500) {
    return Fail(error, L"Video bitrate must be between 1 and 500 Mbps.");
  }

  if (settings.video.gop_seconds <= 0.0 ||
      settings.video.gop_seconds > 10.0) {
    return Fail(error,
                L"Video GOP seconds must be greater than 0 and at most 10.");
  }

  if (settings.video.encoder_backend != L"media_foundation_hardware") {
    return Fail(error, L"The selected video encoder backend is unsupported.");
  }

  if (settings.video.performance_mode !=
          performance::CapturePerformanceMode::Balanced &&
      settings.video.performance_mode !=
          performance::CapturePerformanceMode::CaptureFirst) {
    return Fail(error, L"The selected capture performance mode is invalid.");
  }

  if (settings.audio.sample_rate <= 0) {
    return Fail(error, L"Audio sample rate must be greater than zero.");
  }

  if (settings.clips.presets_seconds.size() != 3) {
    return Fail(error, L"Exactly three clip presets are required.");
  }

  std::set<int> unique_presets;
  for (const int seconds : settings.clips.presets_seconds) {
    if (seconds <= 0 || seconds > 3600) {
      return Fail(error, L"Clip preset durations must be between 1 and 3600 seconds.");
    }
    if (!unique_presets.insert(seconds).second) {
      return Fail(error, L"Clip preset durations must be unique.");
    }
  }

  if (settings.clips.custom_seconds <= 0 ||
      settings.clips.custom_seconds > 3600) {
    return Fail(error,
                L"Custom clip duration must be between 1 and 3600 seconds.");
  }

  if (settings.clips.bookmark_pre_seconds < 0 ||
      settings.clips.bookmark_post_seconds < 0 ||
      (settings.clips.bookmark_pre_seconds == 0 &&
       settings.clips.bookmark_post_seconds == 0)) {
    return Fail(error,
                L"Bookmark export durations must be nonnegative and include "
                L"a positive window.");
  }

  const std::wstring_view hotkey_labels[] = {
      settings.hotkeys.toggle_recording,
      settings.hotkeys.save_last_30s,
      settings.hotkeys.save_last_5m,
      settings.hotkeys.bookmark,
  };
  std::set<std::pair<UINT, UINT>> unique_hotkeys;
  for (const auto label : hotkey_labels) {
    const auto parsed = ParseHotkey(label);
    if (!parsed.Succeeded()) {
      return Fail(error, L"Invalid hotkey '" + std::wstring(label) +
                             L"': " + parsed.message);
    }
    const auto key = std::make_pair(
        parsed.hotkey.modifiers & ~static_cast<UINT>(MOD_NOREPEAT),
        parsed.hotkey.virtual_key);
    if (!unique_hotkeys.insert(key).second) {
      return Fail(error, L"Hotkey bindings must be unique.");
    }
  }

  return true;
}

}  // namespace olouie::settings
