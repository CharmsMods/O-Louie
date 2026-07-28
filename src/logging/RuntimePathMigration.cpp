#include "logging/RuntimePathMigration.h"

#include <windows.h>

#include <cwchar>
#include <system_error>

namespace olouie::logging {
namespace {

bool Exists(const std::filesystem::path& path, bool* exists,
            std::error_code* error) {
  error->clear();
  *exists = std::filesystem::exists(path, *error);
  return !*error;
}

std::wstring NormalizedNative(const std::filesystem::path& path) {
  std::wstring value = path.lexically_normal().native();
  while (value.size() > 3 &&
         (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return value;
}

std::wstring ErrorMessage(const std::error_code& error) {
  const std::string message = error.message();
  return std::wstring(message.begin(), message.end());
}

}  // namespace

bool RuntimePathMigrationResult::NeedsWarning() const noexcept {
  return status == RuntimePathMigrationStatus::LegacyFallback ||
         status == RuntimePathMigrationStatus::RootsConflict ||
         status == RuntimePathMigrationStatus::InspectionFailed;
}

RuntimePathMigrationResult ResolveRuntimePathsWithLegacyMigration(
    std::wstring_view canonical_folder_name,
    std::wstring_view legacy_folder_name) {
  RuntimePathMigrationResult result;
  result.canonical_root =
      RuntimePaths::ForLocalAppData(canonical_folder_name).root;
  result.legacy_root = RuntimePaths::ForLocalAppData(legacy_folder_name).root;
  result.paths = RuntimePaths::FromRoot(result.canonical_root);

  bool canonical_exists = false;
  bool legacy_exists = false;
  std::error_code error;
  if (!Exists(result.canonical_root, &canonical_exists, &error)) {
    result.status = RuntimePathMigrationStatus::InspectionFailed;
    result.message = L"Could not inspect the O'Louie runtime-data folder: " +
                     ErrorMessage(error);
    return result;
  }
  if (!Exists(result.legacy_root, &legacy_exists, &error)) {
    result.status = RuntimePathMigrationStatus::InspectionFailed;
    result.message = L"Could not inspect the legacy runtime-data folder: " +
                     ErrorMessage(error);
    return result;
  }

  if (canonical_exists) {
    if (legacy_exists) {
      result.status = RuntimePathMigrationStatus::RootsConflict;
      result.message =
          L"Both O'Louie and legacy runtime-data folders exist. O'Louie is "
          L"using its current folder and left the legacy folder untouched at '" +
          result.legacy_root.wstring() + L"'.";
    }
    return result;
  }

  if (!legacy_exists) {
    return result;
  }

  error.clear();
  std::filesystem::rename(result.legacy_root, result.canonical_root, error);
  if (!error) {
    result.status = RuntimePathMigrationStatus::Migrated;
    result.message = L"Migrated runtime data to '" +
                     result.canonical_root.wstring() + L"'.";
    return result;
  }

  result.status = RuntimePathMigrationStatus::LegacyFallback;
  result.paths = RuntimePaths::FromRoot(result.legacy_root);
  result.message =
      L"Could not move legacy runtime data to the O'Louie folder. The legacy "
      L"folder remains in use for this run and migration will be retried next "
      L"time. " + ErrorMessage(error);
  return result;
}

bool RebasePathFromLegacyRoot(const std::filesystem::path& path,
                              const std::filesystem::path& legacy_root,
                              const std::filesystem::path& canonical_root,
                              std::filesystem::path* rebased) {
  if (rebased == nullptr || path.empty() || legacy_root.empty() ||
      canonical_root.empty()) {
    return false;
  }

  const std::wstring candidate = NormalizedNative(path);
  const std::wstring legacy = NormalizedNative(legacy_root);
  if (candidate.size() < legacy.size() ||
      _wcsnicmp(candidate.c_str(), legacy.c_str(), legacy.size()) != 0 ||
      (candidate.size() > legacy.size() &&
       candidate[legacy.size()] != L'\\' && candidate[legacy.size()] != L'/')) {
    return false;
  }

  std::wstring suffix = candidate.substr(legacy.size());
  while (!suffix.empty() && (suffix.front() == L'\\' || suffix.front() == L'/')) {
    suffix.erase(suffix.begin());
  }
  *rebased = suffix.empty() ? canonical_root : canonical_root / suffix;
  return true;
}

}  // namespace olouie::logging
