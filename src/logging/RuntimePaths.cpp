#include "logging/RuntimePaths.h"

#include <objbase.h>
#include <shlobj.h>
#include <windows.h>

#include <array>
#include <stdexcept>
#include <string>

namespace olouie::logging {
namespace {

struct CoTaskMemString {
  PWSTR value = nullptr;

  ~CoTaskMemString() {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
  }
};

}  // namespace

RuntimePaths RuntimePaths::FromRoot(const std::filesystem::path& root_path) {
  RuntimePaths paths;
  paths.root = root_path;
  paths.settings = root_path / L"settings";
  paths.logs = root_path / L"logs";
  paths.sessions = root_path / L"sessions";
  paths.exports = root_path / L"exports";
  paths.cache = root_path / L"cache";
  return paths;
}

RuntimePaths RuntimePaths::ForLocalAppData(std::wstring_view app_folder_name) {
  CoTaskMemString local_app_data;
  const HRESULT result =
      SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                           &local_app_data.value);

  if (FAILED(result) || local_app_data.value == nullptr) {
    throw std::runtime_error("SHGetKnownFolderPath(FOLDERID_LocalAppData) failed.");
  }

  return FromRoot(std::filesystem::path(local_app_data.value) /
                  std::wstring(app_folder_name));
}

void RuntimePaths::EnsureCreated() const {
  const std::array directories{root, settings, logs, sessions, exports, cache};
  for (const auto& directory : directories) {
    std::filesystem::create_directories(directory);
  }
}

}  // namespace olouie::logging
