#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "record/ActiveRecordingClip.h"
#include "record/Bookmark.h"

namespace olouie::record {

enum class ActiveRecordingBookmarkStatus {
  Success,
  InvalidRequest,
  PostRollUnsupported,
  ClipPlanFailed,
  BookmarkAddFailed,
  RangeResolutionFailed,
};

struct ActiveRecordingBookmarkOptions {
  int64_t pre_roll_ns = 0;
  int64_t post_roll_ns = 0;
  std::wstring label;
  std::wstring user_note;
  std::filesystem::path temp_output_path;
  std::filesystem::path final_output_path;
  bool allow_overwrite = false;
};

struct ActiveRecordingBookmarkPlan {
  Bookmark bookmark;
  BookmarkExportRange range;
  ActiveRecordingClipPlan clip_plan;

  bool IsReady() const noexcept;
};

struct ActiveRecordingBookmarkResult {
  ActiveRecordingBookmarkStatus status =
      ActiveRecordingBookmarkStatus::InvalidRequest;
  std::wstring message;

  bool Succeeded() const noexcept;
};

ActiveRecordingBookmarkResult BuildActiveRecordingBookmarkPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    BookmarkCollection* bookmarks,
    const ActiveRecordingBookmarkOptions& options,
    ActiveRecordingBookmarkPlan* plan);

const wchar_t* ActiveRecordingBookmarkStatusName(
    ActiveRecordingBookmarkStatus status) noexcept;

}  // namespace olouie::record
