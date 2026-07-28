#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace olouie::record {

using BookmarkId = uint64_t;

struct Bookmark {
  BookmarkId id = 0;
  int64_t time_ns = 0;
  std::wstring label;
  int64_t default_pre_ns = 0;
  int64_t default_post_ns = 0;
  std::wstring user_note;
};

struct BookmarkExportRange {
  BookmarkId bookmark_id = 0;
  int64_t marker_time_ns = 0;
  int64_t requested_start_ns = 0;
  int64_t requested_end_ns = 0;
  int64_t actual_start_ns = 0;
  int64_t actual_end_ns = 0;
  bool clamped_to_session_start = false;
};

bool ValidateBookmark(const Bookmark& bookmark, std::wstring* error);
bool ResolveBookmarkExportRange(
    const Bookmark& bookmark, int64_t session_start_ns,
    BookmarkExportRange* range, std::wstring* error,
    std::optional<int64_t> pre_override_ns = std::nullopt,
    std::optional<int64_t> post_override_ns = std::nullopt);

class BookmarkCollection final {
 public:
  explicit BookmarkCollection(BookmarkId first_id = 1);

  bool Add(int64_t time_ns, int64_t default_pre_ns, int64_t default_post_ns,
           std::wstring label, std::wstring user_note, Bookmark* created,
           std::wstring* error);

  const Bookmark* Find(BookmarkId id) const noexcept;
  std::vector<Bookmark> Snapshot() const;
  size_t Count() const noexcept;

 private:
  BookmarkId next_id_ = 1;
  std::vector<Bookmark> bookmarks_;
};

}  // namespace olouie::record
