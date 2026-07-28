#pragma once

#include <filesystem>
#include <string_view>

namespace olouie::logging {

struct RuntimePaths {
  std::filesystem::path root;
  std::filesystem::path settings;
  std::filesystem::path logs;
  std::filesystem::path sessions;
  std::filesystem::path exports;
  std::filesystem::path cache;

  static RuntimePaths FromRoot(const std::filesystem::path& root_path);
  static RuntimePaths ForLocalAppData(std::wstring_view app_folder_name);

  void EnsureCreated() const;
};

}  // namespace olouie::logging
