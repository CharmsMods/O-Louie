#include "record/ActiveRecordingBookmark.h"

#include <utility>

namespace olouie::record {
namespace {

ActiveRecordingBookmarkResult Result(
    ActiveRecordingBookmarkStatus status,
    std::wstring message) {
  return {status, std::move(message)};
}

}  // namespace

bool ActiveRecordingBookmarkPlan::IsReady() const noexcept {
  return ValidateBookmark(bookmark, nullptr) && clip_plan.IsReady() &&
         range.bookmark_id == bookmark.id &&
         range.marker_time_ns == bookmark.time_ns &&
         bookmark.time_ns == clip_plan.available_end_ns &&
         range.actual_start_ns == clip_plan.clamped_start_ns &&
         range.actual_end_ns == clip_plan.available_end_ns;
}

bool ActiveRecordingBookmarkResult::Succeeded() const noexcept {
  return status == ActiveRecordingBookmarkStatus::Success;
}

ActiveRecordingBookmarkResult BuildActiveRecordingBookmarkPlan(
    const SessionManifest& manifest,
    PacketStore* packet_store,
    BookmarkCollection* bookmarks,
    const ActiveRecordingBookmarkOptions& options,
    ActiveRecordingBookmarkPlan* plan) {
  if (plan == nullptr || packet_store == nullptr || bookmarks == nullptr ||
      options.pre_roll_ns <= 0 || options.post_roll_ns < 0 ||
      options.temp_output_path.empty() || options.final_output_path.empty()) {
    return Result(ActiveRecordingBookmarkStatus::InvalidRequest,
                  L"Active bookmark planning needs a positive pre-roll, "
                  L"nonnegative post-roll, output paths, and destinations.");
  }
  *plan = {};

  if (options.post_roll_ns != 0) {
    return Result(
        ActiveRecordingBookmarkStatus::PostRollUnsupported,
        L"Immediate active bookmark export currently requires zero post-roll.");
  }

  ActiveRecordingClipOptions clip_options;
  clip_options.duration_ns = options.pre_roll_ns;
  clip_options.temp_output_path = options.temp_output_path;
  clip_options.final_output_path = options.final_output_path;
  clip_options.allow_overwrite = options.allow_overwrite;

  ActiveRecordingClipPlan clip_plan;
  const auto clip_result = BuildActiveRecordingClipPlan(
      manifest, packet_store, clip_options, &clip_plan);
  if (!clip_result.Succeeded()) {
    return Result(
        ActiveRecordingBookmarkStatus::ClipPlanFailed,
        clip_result.message.empty()
            ? L"Could not plan the active bookmark export."
            : clip_result.message);
  }

  Bookmark bookmark;
  std::wstring bookmark_error;
  if (!bookmarks->Add(clip_plan.available_end_ns, options.pre_roll_ns,
                      options.post_roll_ns, options.label, options.user_note,
                      &bookmark, &bookmark_error)) {
    return Result(
        ActiveRecordingBookmarkStatus::BookmarkAddFailed,
        bookmark_error.empty() ? L"Could not add the active bookmark."
                               : std::move(bookmark_error));
  }

  BookmarkExportRange range;
  if (!ResolveBookmarkExportRange(bookmark, 0, &range, &bookmark_error)) {
    return Result(
        ActiveRecordingBookmarkStatus::RangeResolutionFailed,
        bookmark_error.empty()
            ? L"Could not resolve the active bookmark export range."
            : std::move(bookmark_error));
  }

  ActiveRecordingBookmarkPlan built;
  built.bookmark = std::move(bookmark);
  built.range = range;
  built.clip_plan = std::move(clip_plan);
  if (!built.IsReady()) {
    return Result(ActiveRecordingBookmarkStatus::RangeResolutionFailed,
                  L"Active bookmark range does not match its live clip plan.");
  }

  *plan = std::move(built);
  return Result(ActiveRecordingBookmarkStatus::Success, L"");
}

const wchar_t* ActiveRecordingBookmarkStatusName(
    ActiveRecordingBookmarkStatus status) noexcept {
  switch (status) {
    case ActiveRecordingBookmarkStatus::Success:
      return L"success";
    case ActiveRecordingBookmarkStatus::InvalidRequest:
      return L"invalid request";
    case ActiveRecordingBookmarkStatus::PostRollUnsupported:
      return L"post-roll unsupported";
    case ActiveRecordingBookmarkStatus::ClipPlanFailed:
      return L"clip plan failed";
    case ActiveRecordingBookmarkStatus::BookmarkAddFailed:
      return L"bookmark add failed";
    case ActiveRecordingBookmarkStatus::RangeResolutionFailed:
      return L"range resolution failed";
  }
  return L"unknown";
}

}  // namespace olouie::record
