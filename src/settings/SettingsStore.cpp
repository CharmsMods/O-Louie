#include "settings/SettingsStore.h"

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

namespace olouie::settings {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr uintmax_t kMaximumSettingsFileBytes = 1024u * 1024u;

SettingsLoadResult LoadResult(SettingsLoadStatus status,
                              std::wstring message) {
  SettingsLoadResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

SettingsSaveResult SaveResult(SettingsSaveStatus status,
                              std::wstring message) {
  SettingsSaveResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

int ReadInteger(const JsonObject& object, std::wstring_view name) {
  const double number = object.GetNamedNumber(name);
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(std::numeric_limits<int>::min()) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    throw winrt::hresult_invalid_argument();
  }
  return static_cast<int>(number);
}

uint32_t ReadVersion(const JsonObject& object) {
  const double number = object.GetNamedNumber(L"version");
  if (!std::isfinite(number) || std::floor(number) != number || number < 0 ||
      number > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    throw winrt::hresult_invalid_argument();
  }
  return static_cast<uint32_t>(number);
}

std::wstring ReadString(const JsonObject& object, std::wstring_view name) {
  return std::wstring(object.GetNamedString(name));
}

ResolutionMode ReadResolutionMode(const JsonObject& object) {
  const std::wstring value = ReadString(object, L"resolution_mode");
  if (value == L"source") {
    return ResolutionMode::Source;
  }
  if (value == L"custom") {
    return ResolutionMode::Custom;
  }
  throw winrt::hresult_invalid_argument();
}

const wchar_t* ResolutionModeValue(ResolutionMode mode) {
  switch (mode) {
    case ResolutionMode::Source:
      return L"source";
    case ResolutionMode::Custom:
      return L"custom";
  }
  return L"source";
}

performance::CapturePerformanceMode ReadCapturePerformanceMode(
    const JsonObject& object) {
  const std::wstring value = ReadString(object, L"performance_mode");
  if (value == L"balanced") {
    return performance::CapturePerformanceMode::Balanced;
  }
  if (value == L"capture_first") {
    return performance::CapturePerformanceMode::CaptureFirst;
  }
  throw winrt::hresult_invalid_argument();
}

const wchar_t* CapturePerformanceModeValue(
    performance::CapturePerformanceMode mode) {
  switch (mode) {
    case performance::CapturePerformanceMode::Balanced:
      return L"balanced";
    case performance::CapturePerformanceMode::CaptureFirst:
      return L"capture_first";
  }
  return L"balanced";
}

AppSettings ParseCurrentSettings(const JsonObject& root, uint32_t version) {
  AppSettings settings;
  settings.output_directory = ReadString(root, L"output_directory");
  settings.start_with_windows = root.GetNamedBoolean(L"start_with_windows");
  settings.show_overlay_notifications =
      root.GetNamedBoolean(L"show_overlay_notifications");

  const JsonObject video = root.GetNamedObject(L"video");
  settings.video.monitor_device_name =
      ReadString(video, L"monitor_device_name");
  settings.video.resolution_mode = ReadResolutionMode(video);
  settings.video.custom_width = ReadInteger(video, L"custom_width");
  settings.video.custom_height = ReadInteger(video, L"custom_height");
  settings.video.fps = ReadInteger(video, L"fps");
  settings.video.bitrate_mbps = ReadInteger(video, L"bitrate_mbps");
  settings.video.gop_seconds = video.GetNamedNumber(L"gop_seconds");
  settings.video.capture_cursor = video.GetNamedBoolean(L"capture_cursor");
  if (version >= 3) {
    settings.video.performance_mode = ReadCapturePerformanceMode(video);
  }
  settings.video.encoder_backend = ReadString(video, L"encoder_backend");

  const JsonObject audio = root.GetNamedObject(L"audio");
  settings.audio.system_mix = audio.GetNamedBoolean(L"system_mix");
  settings.audio.mic = audio.GetNamedBoolean(L"mic");
  settings.audio.separate_tracks = audio.GetNamedBoolean(L"separate_tracks");
  settings.audio.default_mixed_track =
      audio.GetNamedBoolean(L"default_mixed_track");
  settings.audio.sample_rate = ReadInteger(audio, L"sample_rate");
  if (version >= 2) {
    settings.audio.mic_check_output_device_id =
        ReadString(audio, L"mic_check_output_device_id");
  }

  const JsonObject clips = root.GetNamedObject(L"clips");
  const JsonArray presets = clips.GetNamedArray(L"presets_seconds");
  settings.clips.presets_seconds.clear();
  settings.clips.presets_seconds.reserve(presets.Size());
  for (uint32_t index = 0; index < presets.Size(); ++index) {
    const double number = presets.GetNumberAt(index);
    if (!std::isfinite(number) || std::floor(number) != number || number < 0 ||
        number > static_cast<double>(std::numeric_limits<int>::max())) {
      throw winrt::hresult_invalid_argument();
    }
    settings.clips.presets_seconds.push_back(static_cast<int>(number));
  }
  settings.clips.custom_seconds = ReadInteger(clips, L"custom_seconds");
  settings.clips.bookmark_pre_seconds =
      ReadInteger(clips, L"bookmark_pre_seconds");
  settings.clips.bookmark_post_seconds =
      ReadInteger(clips, L"bookmark_post_seconds");

  const JsonObject hotkeys = root.GetNamedObject(L"hotkeys");
  settings.hotkeys.toggle_recording =
      ReadString(hotkeys, L"toggle_recording");
  settings.hotkeys.save_last_30s = ReadString(hotkeys, L"save_last_30s");
  settings.hotkeys.save_last_5m = ReadString(hotkeys, L"save_last_5m");
  settings.hotkeys.bookmark = ReadString(hotkeys, L"bookmark");
  return settings;
}

void SetString(JsonObject& object, std::wstring_view name,
               std::wstring_view value) {
  object.SetNamedValue(name, JsonValue::CreateStringValue(value));
}

void SetNumber(JsonObject& object, std::wstring_view name, double value) {
  object.SetNamedValue(name, JsonValue::CreateNumberValue(value));
}

void SetBoolean(JsonObject& object, std::wstring_view name, bool value) {
  object.SetNamedValue(name, JsonValue::CreateBooleanValue(value));
}

JsonObject SerializeSettings(const AppSettings& settings) {
  JsonObject root;
  SetNumber(root, L"version", kSettingsFileVersion);
  SetString(root, L"output_directory", settings.output_directory.native());
  SetBoolean(root, L"start_with_windows", settings.start_with_windows);
  SetBoolean(root, L"show_overlay_notifications",
             settings.show_overlay_notifications);

  JsonObject video;
  SetString(video, L"monitor_device_name",
            settings.video.monitor_device_name);
  SetString(video, L"resolution_mode",
            ResolutionModeValue(settings.video.resolution_mode));
  SetNumber(video, L"custom_width", settings.video.custom_width);
  SetNumber(video, L"custom_height", settings.video.custom_height);
  SetNumber(video, L"fps", settings.video.fps);
  SetNumber(video, L"bitrate_mbps", settings.video.bitrate_mbps);
  SetNumber(video, L"gop_seconds", settings.video.gop_seconds);
  SetBoolean(video, L"capture_cursor", settings.video.capture_cursor);
  SetString(video, L"performance_mode",
            CapturePerformanceModeValue(settings.video.performance_mode));
  SetString(video, L"encoder_backend", settings.video.encoder_backend);
  root.SetNamedValue(L"video", video);

  JsonObject audio;
  SetBoolean(audio, L"system_mix", settings.audio.system_mix);
  SetBoolean(audio, L"mic", settings.audio.mic);
  SetBoolean(audio, L"separate_tracks", settings.audio.separate_tracks);
  SetBoolean(audio, L"default_mixed_track",
             settings.audio.default_mixed_track);
  SetNumber(audio, L"sample_rate", settings.audio.sample_rate);
  SetString(audio, L"mic_check_output_device_id",
            settings.audio.mic_check_output_device_id);
  root.SetNamedValue(L"audio", audio);

  JsonObject clips;
  JsonArray presets;
  for (const int seconds : settings.clips.presets_seconds) {
    presets.Append(JsonValue::CreateNumberValue(seconds));
  }
  clips.SetNamedValue(L"presets_seconds", presets);
  SetNumber(clips, L"custom_seconds", settings.clips.custom_seconds);
  SetNumber(clips, L"bookmark_pre_seconds",
            settings.clips.bookmark_pre_seconds);
  SetNumber(clips, L"bookmark_post_seconds",
            settings.clips.bookmark_post_seconds);
  root.SetNamedValue(L"clips", clips);

  JsonObject hotkeys;
  SetString(hotkeys, L"toggle_recording",
            settings.hotkeys.toggle_recording);
  SetString(hotkeys, L"save_last_30s", settings.hotkeys.save_last_30s);
  SetString(hotkeys, L"save_last_5m", settings.hotkeys.save_last_5m);
  SetString(hotkeys, L"bookmark", settings.hotkeys.bookmark);
  root.SetNamedValue(L"hotkeys", hotkeys);
  return root;
}

}  // namespace

bool SettingsLoadResult::LoadedFromDisk() const noexcept {
  return status == SettingsLoadStatus::Loaded;
}

bool SettingsLoadResult::UsedDefaults() const noexcept {
  return !LoadedFromDisk();
}

bool SettingsSaveResult::Succeeded() const noexcept {
  return status == SettingsSaveStatus::Saved;
}

SettingsLoadResult LoadSettingsFile(const std::filesystem::path& path,
                                    const AppSettings& defaults,
                                    AppSettings* settings) {
  if (settings == nullptr) {
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"Settings output storage is null.");
  }
  *settings = defaults;

  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    const std::string narrow_message = error.message();
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"Could not inspect the settings file: " +
                          std::wstring(narrow_message.begin(),
                                       narrow_message.end()));
  }
  if (!exists) {
    return LoadResult(SettingsLoadStatus::MissingUsingDefaults,
                      L"Settings file is missing; using defaults.");
  }
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"The settings path is not a readable file; using defaults.");
  }
  const uintmax_t file_size = std::filesystem::file_size(path, error);
  if (error || file_size > kMaximumSettingsFileBytes) {
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"The settings file is unavailable or too large; using defaults.");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"Could not open the settings file; using defaults.");
  }
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    return LoadResult(SettingsLoadStatus::IoErrorUsingDefaults,
                      L"Could not read the settings file; using defaults.");
  }

  try {
    const JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
    const uint32_t version = ReadVersion(root);
    if (version == 0 || version > kSettingsFileVersion) {
      return LoadResult(
          SettingsLoadStatus::UnsupportedVersionUsingDefaults,
          L"Settings file version " + std::to_wstring(version) +
              L" is unsupported; using defaults without overwriting it.");
    }

    AppSettings parsed = ParseCurrentSettings(root, version);
    std::wstring validation_error;
    if (!Validate(parsed, &validation_error)) {
      return LoadResult(SettingsLoadStatus::InvalidUsingDefaults,
                        L"Settings validation failed; using defaults: " +
                            validation_error);
    }
    *settings = std::move(parsed);
    return LoadResult(SettingsLoadStatus::Loaded, L"Settings loaded.");
  } catch (const winrt::hresult_error& exception) {
    return LoadResult(SettingsLoadStatus::MalformedUsingDefaults,
                      L"Settings JSON is malformed or incomplete; using defaults (" +
                          std::wstring(exception.message()) + L").");
  } catch (const std::exception&) {
    return LoadResult(SettingsLoadStatus::MalformedUsingDefaults,
                      L"Settings JSON is malformed or incomplete; using defaults.");
  }
}

SettingsSaveResult SaveSettingsFileAtomic(const std::filesystem::path& path,
                                          const AppSettings& settings) {
  std::wstring validation_error;
  if (!Validate(settings, &validation_error)) {
    return SaveResult(SettingsSaveStatus::Invalid, validation_error);
  }
  if (path.empty() || path.filename().empty()) {
    return SaveResult(SettingsSaveStatus::IoError,
                      L"Settings file path is invalid.");
  }

  try {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return SaveResult(SettingsSaveStatus::IoError,
                        L"Could not create the settings directory.");
    }

    const std::string json =
        winrt::to_string(SerializeSettings(settings).Stringify()) + "\n";
    std::filesystem::path temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::remove(temporary, error);
    error.clear();

    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) {
        return SaveResult(SettingsSaveStatus::IoError,
                          L"Could not create the temporary settings file.");
      }
      output.write(json.data(), static_cast<std::streamsize>(json.size()));
      output.flush();
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        return SaveResult(SettingsSaveStatus::IoError,
                          L"Could not write the temporary settings file.");
      }
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD move_error = GetLastError();
      std::filesystem::remove(temporary, error);
      return SaveResult(SettingsSaveStatus::IoError,
                        L"Could not atomically replace settings.json (Win32 error " +
                            std::to_wstring(move_error) + L").");
    }
    return SaveResult(SettingsSaveStatus::Saved, L"Settings saved.");
  } catch (const winrt::hresult_error& exception) {
    return SaveResult(SettingsSaveStatus::IoError,
                      L"Could not serialize settings: " +
                          std::wstring(exception.message()) + L".");
  } catch (const std::exception&) {
    return SaveResult(SettingsSaveStatus::IoError,
                      L"Could not save settings.");
  }
}

const wchar_t* SettingsLoadStatusName(SettingsLoadStatus status) noexcept {
  switch (status) {
    case SettingsLoadStatus::Loaded:
      return L"loaded";
    case SettingsLoadStatus::MissingUsingDefaults:
      return L"missing; defaults used";
    case SettingsLoadStatus::MalformedUsingDefaults:
      return L"malformed; defaults used";
    case SettingsLoadStatus::UnsupportedVersionUsingDefaults:
      return L"unsupported version; defaults used";
    case SettingsLoadStatus::InvalidUsingDefaults:
      return L"invalid; defaults used";
    case SettingsLoadStatus::IoErrorUsingDefaults:
      return L"I/O error; defaults used";
  }
  return L"unknown";
}

const wchar_t* SettingsSaveStatusName(SettingsSaveStatus status) noexcept {
  switch (status) {
    case SettingsSaveStatus::Saved:
      return L"saved";
    case SettingsSaveStatus::Invalid:
      return L"invalid";
    case SettingsSaveStatus::IoError:
      return L"I/O error";
  }
  return L"unknown";
}

}  // namespace olouie::settings
