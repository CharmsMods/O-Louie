#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "settings/Settings.h"

namespace olouie::settings {

inline constexpr uint32_t kSettingsFileVersion = 3;

enum class SettingsLoadStatus {
  Loaded,
  MissingUsingDefaults,
  MalformedUsingDefaults,
  UnsupportedVersionUsingDefaults,
  InvalidUsingDefaults,
  IoErrorUsingDefaults,
};

struct SettingsLoadResult {
  SettingsLoadStatus status = SettingsLoadStatus::IoErrorUsingDefaults;
  std::wstring message;

  bool LoadedFromDisk() const noexcept;
  bool UsedDefaults() const noexcept;
};

enum class SettingsSaveStatus {
  Saved,
  Invalid,
  IoError,
};

struct SettingsSaveResult {
  SettingsSaveStatus status = SettingsSaveStatus::IoError;
  std::wstring message;

  bool Succeeded() const noexcept;
};

SettingsLoadResult LoadSettingsFile(const std::filesystem::path& path,
                                    const AppSettings& defaults,
                                    AppSettings* settings);
SettingsSaveResult SaveSettingsFileAtomic(const std::filesystem::path& path,
                                          const AppSettings& settings);

const wchar_t* SettingsLoadStatusName(SettingsLoadStatus status) noexcept;
const wchar_t* SettingsSaveStatusName(SettingsSaveStatus status) noexcept;

}  // namespace olouie::settings
