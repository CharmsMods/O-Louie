#include "record/Bookmark.h"

#include <algorithm>
#include <string>

namespace olouie::record {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

std::wstring DefaultLabel(BookmarkId id) {
  return L"Bookmark " + std::to_wstring(id);
}

}  // namespace

bool ValidateBookmark(const Bookmark& bookmark, std::wstring* error) {
  if (bookmark.id == 0) {
    SetError(error, L"Bookmark id must be nonzero.");
    return false;
  }

  if (bookmark.time_ns < 0) {
    SetError(error, L"Bookmark time must not be negative.");
    return false;
  }

  if (bookmark.default_pre_ns < 0 || bookmark.default_post_ns < 0) {
    SetError(error, L"Bookmark default export durations must not be negative.");
    return false;
  }

  if (bookmark.default_pre_ns + bookmark.default_post_ns <= 0) {
    SetError(error, L"Bookmark default export duration must be greater than zero.");
    return false;
  }

  if (bookmark.label.empty()) {
    SetError(error, L"Bookmark label must not be empty.");
    return false;
  }

  return true;
}

bool ResolveBookmarkExportRange(const Bookmark& bookmark,
                                int64_t session_start_ns,
                                BookmarkExportRange* range,
                                std::wstring* error,
                                std::optional<int64_t> pre_override_ns,
                                std::optional<int64_t> post_override_ns) {
  if (range == nullptr) {
    SetError(error, L"Bookmark export range needs an output destination.");
    return false;
  }

  if (!ValidateBookmark(bookmark, error)) {
    return false;
  }

  if (session_start_ns < 0) {
    SetError(error, L"Session start must not be negative.");
    return false;
  }

  const int64_t pre_ns = pre_override_ns.value_or(bookmark.default_pre_ns);
  const int64_t post_ns = post_override_ns.value_or(bookmark.default_post_ns);

  if (pre_ns < 0 || post_ns < 0) {
    SetError(error, L"Bookmark export durations must not be negative.");
    return false;
  }

  if (pre_ns + post_ns <= 0) {
    SetError(error, L"Bookmark export duration must be greater than zero.");
    return false;
  }

  BookmarkExportRange resolved;
  resolved.bookmark_id = bookmark.id;
  resolved.marker_time_ns = bookmark.time_ns;
  resolved.requested_start_ns = bookmark.time_ns - pre_ns;
  resolved.requested_end_ns = bookmark.time_ns + post_ns;
  resolved.actual_start_ns = std::max(session_start_ns, resolved.requested_start_ns);
  resolved.actual_end_ns = resolved.requested_end_ns;
  resolved.clamped_to_session_start =
      resolved.actual_start_ns != resolved.requested_start_ns;

  if (resolved.actual_end_ns <= resolved.actual_start_ns) {
    SetError(error, L"Bookmark export range is empty after session clamping.");
    return false;
  }

  *range = resolved;
  return true;
}

BookmarkCollection::BookmarkCollection(BookmarkId first_id)
    : next_id_(std::max<BookmarkId>(1, first_id)) {}

bool BookmarkCollection::Add(int64_t time_ns, int64_t default_pre_ns,
                             int64_t default_post_ns, std::wstring label,
                             std::wstring user_note, Bookmark* created,
                             std::wstring* error) {
  Bookmark bookmark;
  bookmark.id = next_id_;
  bookmark.time_ns = time_ns;
  bookmark.label = label.empty() ? DefaultLabel(bookmark.id) : std::move(label);
  bookmark.default_pre_ns = default_pre_ns;
  bookmark.default_post_ns = default_post_ns;
  bookmark.user_note = std::move(user_note);

  if (!ValidateBookmark(bookmark, error)) {
    return false;
  }

  bookmarks_.push_back(bookmark);
  ++next_id_;

  if (created != nullptr) {
    *created = std::move(bookmark);
  }

  return true;
}

const Bookmark* BookmarkCollection::Find(BookmarkId id) const noexcept {
  const auto found = std::find_if(
      bookmarks_.begin(), bookmarks_.end(),
      [id](const Bookmark& bookmark) { return bookmark.id == id; });
  return found == bookmarks_.end() ? nullptr : &(*found);
}

std::vector<Bookmark> BookmarkCollection::Snapshot() const {
  return bookmarks_;
}

size_t BookmarkCollection::Count() const noexcept {
  return bookmarks_.size();
}

}  // namespace olouie::record
