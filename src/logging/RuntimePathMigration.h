#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "logging/RuntimePaths.h"

namespace olouie::logging {

enum class RuntimePathMigrationStatus {
  NotNeeded,
  Migrated,
  LegacyFallback,
  RootsConflict,
  InspectionFailed,
};

struct RuntimePathMigrationResult {
  RuntimePathMigrationStatus status = RuntimePathMigrationStatus::NotNeeded;
  RuntimePaths paths;
  std::filesystem::path canonical_root;
  std::filesystem::path legacy_root;
  std::wstring message;

  bool NeedsWarning() const noexcept;
};

RuntimePathMigrationResult ResolveRuntimePathsWithLegacyMigration(
    std::wstring_view canonical_folder_name,
    std::wstring_view legacy_folder_name);

bool RebasePathFromLegacyRoot(const std::filesystem::path& path,
                              const std::filesystem::path& legacy_root,
                              const std::filesystem::path& canonical_root,
                              std::filesystem::path* rebased);

}  // namespace olouie::logging
