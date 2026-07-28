#include "record/SessionManifest.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace olouie::record {
namespace {

SessionManifestResult Result(SessionManifestStatus status,
                             std::wstring message,
                             DiskWriteFault write_fault = {}) {
  SessionManifestResult result;
  result.status = status;
  result.message = std::move(message);
  result.write_fault = std::move(write_fault);
  return result;
}

std::error_code StreamErrorCode() {
  if (errno != 0) {
    return {errno, std::generic_category()};
  }
  return std::make_error_code(std::errc::io_error);
}

SessionManifestResult TemporaryWriteFailure(
    const std::filesystem::path& temp_path,
    DiskWriteOperation operation,
    std::error_code stream_error,
    std::wstring detail) {
  auto fault = MakeDiskWriteFault(
      DiskWriteSubsystem::SessionManifest, operation, temp_path,
      stream_error, 0, std::move(detail));
  std::wstring message = DescribeDiskWriteFault(fault);
  std::error_code cleanup_error;
  std::filesystem::remove(temp_path, cleanup_error);
  if (cleanup_error) {
    const auto cleanup_fault = MakeDiskWriteFault(
        DiskWriteSubsystem::SessionManifest,
        DiskWriteOperation::RemoveFailedPartial, temp_path, cleanup_error);
    message += L" Cleanup: " + DescribeDiskWriteFault(cleanup_fault);
  }
  return Result(SessionManifestStatus::WriteFailed, std::move(message),
                std::move(fault));
}

bool IsHexDigit(wchar_t ch) noexcept {
  return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
         (ch >= L'A' && ch <= L'F');
}

uint8_t HexValue(wchar_t ch) noexcept {
  if (ch >= L'0' && ch <= L'9') {
    return static_cast<uint8_t>(ch - L'0');
  }
  if (ch >= L'a' && ch <= L'f') {
    return static_cast<uint8_t>(10 + ch - L'a');
  }
  return static_cast<uint8_t>(10 + ch - L'A');
}

std::wstring BytesToHex(const std::vector<uint8_t>& bytes) {
  constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring hex;
  hex.reserve(bytes.size() * 2u);
  for (const auto byte : bytes) {
    hex.push_back(kHex[(byte >> 4u) & 0x0Fu]);
    hex.push_back(kHex[byte & 0x0Fu]);
  }
  return hex;
}

bool HexToBytes(const std::wstring& hex, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr || (hex.size() % 2u) != 0) {
    return false;
  }

  std::vector<uint8_t> parsed;
  parsed.reserve(hex.size() / 2u);
  for (size_t index = 0; index < hex.size(); index += 2u) {
    if (!IsHexDigit(hex[index]) || !IsHexDigit(hex[index + 1u])) {
      return false;
    }
    parsed.push_back(static_cast<uint8_t>(
        (HexValue(hex[index]) << 4u) | HexValue(hex[index + 1u])));
  }

  *bytes = std::move(parsed);
  return true;
}

void WriteJsonString(std::ostream& output, std::wstring_view value) {
  output << '"';
  for (const wchar_t ch : value) {
    switch (ch) {
      case L'"':
        output << "\\\"";
        break;
      case L'\\':
        output << "\\\\";
        break;
      case L'\b':
        output << "\\b";
        break;
      case L'\f':
        output << "\\f";
        break;
      case L'\n':
        output << "\\n";
        break;
      case L'\r':
        output << "\\r";
        break;
      case L'\t':
        output << "\\t";
        break;
      default:
        if (ch >= 0x20 && ch <= 0x7E) {
          output << static_cast<char>(ch);
        } else {
          output << "\\u" << std::hex << std::setw(4)
                 << std::setfill('0') << static_cast<uint32_t>(ch)
                 << std::dec << std::setfill(' ');
        }
        break;
    }
  }
  output << '"';
}

void WriteField(std::ostream& output,
                const char* key,
                std::wstring_view value,
                bool last = false) {
  output << "  \"" << key << "\": ";
  WriteJsonString(output, value);
  output << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                const wchar_t* value,
                bool last = false) {
  WriteField(output, key, std::wstring_view(value), last);
}

void WritePathField(std::ostream& output,
                    const char* key,
                    const std::filesystem::path& value,
                    bool last = false) {
  WriteField(output, key, value.native(), last);
}

void WriteField(std::ostream& output,
                const char* key,
                uint32_t value,
                bool last = false) {
  output << "  \"" << key << "\": " << value << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                uint64_t value,
                bool last = false) {
  output << "  \"" << key << "\": " << value << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                int32_t value,
                bool last = false) {
  output << "  \"" << key << "\": " << value << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                int64_t value,
                bool last = false) {
  output << "  \"" << key << "\": " << value << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                double value,
                bool last = false) {
  output << "  \"" << key << "\": " << std::setprecision(17) << value
         << (last ? "\n" : ",\n");
}

void WriteField(std::ostream& output,
                const char* key,
                bool value,
                bool last = false) {
  output << "  \"" << key << "\": " << (value ? "true" : "false")
         << (last ? "\n" : ",\n");
}

bool FindValueStart(std::string_view json,
                    std::string_view key,
                    size_t* value_start) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string_view::npos) {
    return false;
  }

  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string_view::npos) {
    return false;
  }

  size_t pos = colon_pos + 1u;
  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
    ++pos;
  }
  if (pos >= json.size()) {
    return false;
  }

  *value_start = pos;
  return true;
}

bool ParseUnicodeEscape(std::string_view json, size_t* pos, wchar_t* output) {
  if (*pos + 4u > json.size()) {
    return false;
  }

  uint32_t value = 0;
  for (size_t index = 0; index < 4u; ++index) {
    const char ch = json[*pos + index];
    value <<= 4u;
    if (ch >= '0' && ch <= '9') {
      value += static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value += static_cast<uint32_t>(10 + ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
      value += static_cast<uint32_t>(10 + ch - 'A');
    } else {
      return false;
    }
  }

  *pos += 4u;
  *output = static_cast<wchar_t>(value);
  return true;
}

bool ParseJsonString(std::string_view json, size_t start, std::wstring* value) {
  if (value == nullptr || start >= json.size() || json[start] != '"') {
    return false;
  }

  std::wstring parsed;
  for (size_t pos = start + 1u; pos < json.size();) {
    const char ch = json[pos++];
    if (ch == '"') {
      *value = std::move(parsed);
      return true;
    }

    if (ch != '\\') {
      parsed.push_back(static_cast<unsigned char>(ch));
      continue;
    }

    if (pos >= json.size()) {
      return false;
    }
    const char escaped = json[pos++];
    switch (escaped) {
      case '"':
        parsed.push_back(L'"');
        break;
      case '\\':
        parsed.push_back(L'\\');
        break;
      case '/':
        parsed.push_back(L'/');
        break;
      case 'b':
        parsed.push_back(L'\b');
        break;
      case 'f':
        parsed.push_back(L'\f');
        break;
      case 'n':
        parsed.push_back(L'\n');
        break;
      case 'r':
        parsed.push_back(L'\r');
        break;
      case 't':
        parsed.push_back(L'\t');
        break;
      case 'u': {
        wchar_t unicode = 0;
        if (!ParseUnicodeEscape(json, &pos, &unicode)) {
          return false;
        }
        parsed.push_back(unicode);
        break;
      }
      default:
        return false;
    }
  }

  return false;
}

bool ReadString(std::string_view json,
                std::string_view key,
                std::wstring* value) {
  size_t start = 0;
  return FindValueStart(json, key, &start) && ParseJsonString(json, start, value);
}

bool ReadUint32(std::string_view json,
                std::string_view key,
                uint32_t* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  size_t end = start;
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) != 0)) {
    ++end;
  }
  if (end == start) {
    return false;
  }

  uint32_t parsed = 0;
  const auto result =
      std::from_chars(json.data() + start, json.data() + end, parsed);
  if (result.ec != std::errc{}) {
    return false;
  }

  *value = parsed;
  return true;
}

bool ReadUint64(std::string_view json,
                std::string_view key,
                uint64_t* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  size_t end = start;
  while (end < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[end])) != 0) {
    ++end;
  }
  if (end == start) {
    return false;
  }

  uint64_t parsed = 0;
  const auto result =
      std::from_chars(json.data() + start, json.data() + end, parsed);
  if (result.ec != std::errc{}) {
    return false;
  }

  *value = parsed;
  return true;
}

bool ReadInt32(std::string_view json,
               std::string_view key,
               int32_t* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  size_t end = start;
  if (json[end] == '-') {
    ++end;
  }
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) != 0)) {
    ++end;
  }
  if (end == start || (end == start + 1u && json[start] == '-')) {
    return false;
  }

  int32_t parsed = 0;
  const auto result =
      std::from_chars(json.data() + start, json.data() + end, parsed);
  if (result.ec != std::errc{}) {
    return false;
  }

  *value = parsed;
  return true;
}

bool ReadInt64(std::string_view json,
               std::string_view key,
               int64_t* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  size_t end = start;
  if (json[end] == '-') {
    ++end;
  }
  while (end < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[end])) != 0) {
    ++end;
  }
  if (end == start || (end == start + 1u && json[start] == '-')) {
    return false;
  }

  int64_t parsed = 0;
  const auto result =
      std::from_chars(json.data() + start, json.data() + end, parsed);
  if (result.ec != std::errc{}) {
    return false;
  }

  *value = parsed;
  return true;
}

bool ReadDouble(std::string_view json,
                std::string_view key,
                double* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  size_t end = start;
  while (end < json.size()) {
    const char ch = json[end];
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0 && ch != '-' &&
        ch != '+' && ch != '.' && ch != 'e' && ch != 'E') {
      break;
    }
    ++end;
  }
  if (end == start) {
    return false;
  }

  try {
    *value = std::stod(std::string(json.substr(start, end - start)));
  } catch (...) {
    return false;
  }
  return true;
}

bool ReadBool(std::string_view json, std::string_view key, bool* value) {
  size_t start = 0;
  if (value == nullptr || !FindValueStart(json, key, &start)) {
    return false;
  }

  if (json.substr(start, 4u) == "true") {
    *value = true;
    return true;
  }
  if (json.substr(start, 5u) == "false") {
    *value = false;
    return true;
  }
  return false;
}

bool ReadBytes(std::string_view json,
               std::string_view key,
               std::vector<uint8_t>* bytes) {
  std::wstring hex;
  return ReadString(json, key, &hex) && HexToBytes(hex, bytes);
}

bool ReadCodec(std::string_view json, std::string_view key,
               CodecId* codec_id) {
  std::wstring codec;
  if (!ReadString(json, key, &codec)) {
    return false;
  }

  if (codec == L"h264") {
    *codec_id = CodecId::H264;
    return true;
  }
  if (codec == L"aac") {
    *codec_id = CodecId::Aac;
    return true;
  }
  return false;
}

bool IsKnownAudioSourceKind(std::wstring_view source_kind) noexcept {
  return source_kind == L"default_mixed" ||
         source_kind == L"system_loopback" ||
         source_kind == L"microphone" ||
         source_kind == L"process_loopback";
}

std::string AudioKey(size_t index, std::string_view suffix) {
  return "audio_" + std::to_string(index) + "_" + std::string(suffix);
}

std::string BookmarkKey(size_t index, std::string_view suffix) {
  return "bookmark_" + std::to_string(index) + "_" + std::string(suffix);
}

}  // namespace

bool VideoTrackSessionManifest::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::H264 &&
         h264_packet_format == L"annex_b" && !h264_sps.empty() &&
         !h264_pps.empty() && !h264_avcc_extradata.empty() &&
         requested_width > 0 && requested_height > 0 &&
         requested_fps_numerator > 0 && requested_fps_denominator > 0 &&
         requested_bitrate_bps > 0 && requested_gop_seconds > 0.0 &&
         media_width > 0 && media_height > 0 && media_fps_numerator > 0 &&
         media_fps_denominator > 0 && media_bitrate_bps > 0 &&
         source_width > 0 && source_height > 0 && output_width > 0 &&
         output_height > 0;
}

bool AudioTrackSessionManifest::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::Aac &&
         IsKnownAudioSourceKind(source_kind) && !name.empty() &&
         sample_rate > 0 && channel_count > 0 && bitrate_bps > 0 &&
         aac_frame_samples == 1024 && aac_payload_type == 0 &&
         aac_audio_object_type == 2 &&
         !aac_audio_specific_config.empty() && !encoder_name.empty();
}

bool SessionManifest::IsReady() const noexcept {
  if ((version != kLegacyVideoOnlySessionManifestVersion &&
       version != kAudioSessionManifestVersion &&
       version != kSessionManifestVersion) ||
      session_dir.empty() || packet_file_path.empty() || !video.IsReady() ||
      audio_tracks.size() > std::numeric_limits<uint32_t>::max() ||
      bookmarks.size() > std::numeric_limits<uint32_t>::max() ||
      (version == kLegacyVideoOnlySessionManifestVersion &&
       (!audio_tracks.empty() || !bookmarks.empty())) ||
      (version == kAudioSessionManifestVersion && !bookmarks.empty())) {
    return false;
  }

  std::set<uint32_t> track_ids{video.track_id};
  for (const auto& audio : audio_tracks) {
    if (!audio.IsReady() || !track_ids.insert(audio.track_id).second) {
      return false;
    }
  }
  std::set<BookmarkId> bookmark_ids;
  for (const auto& bookmark : bookmarks) {
    if (!ValidateBookmark(bookmark, nullptr) ||
        !bookmark_ids.insert(bookmark.id).second) {
      return false;
    }
  }
  return true;
}

bool SessionManifestResult::Succeeded() const noexcept {
  return status == SessionManifestStatus::Success;
}

std::filesystem::path SessionManifestPath(
    const std::filesystem::path& session_dir) {
  return session_dir / L"session.json";
}

SessionManifestResult WriteSessionManifest(
    const SessionManifest& manifest) {
  if (!manifest.IsReady() || manifest.version != kSessionManifestVersion) {
    return Result(SessionManifestStatus::InvalidConfig,
                  L"Session manifest is missing required current-version "
                  L"video, audio, or bookmark metadata.");
  }

  std::error_code error_code;
  std::filesystem::create_directories(manifest.session_dir, error_code);
  if (error_code) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::SessionManifest,
        DiskWriteOperation::CreateDirectories, manifest.session_dir,
        error_code);
    return Result(SessionManifestStatus::WriteFailed,
                  DescribeDiskWriteFault(fault), fault);
  }

  const auto path = SessionManifestPath(manifest.session_dir);
  auto temp_path = path;
  temp_path += L".tmp";

  errno = 0;
  std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::SessionManifest,
        DiskWriteOperation::OpenTemporaryFile, temp_path,
        StreamErrorCode());
    return Result(SessionManifestStatus::WriteFailed,
                  DescribeDiskWriteFault(fault), fault);
  }

  errno = 0;
  const auto& video = manifest.video;
  output << "{\n";
  WriteField(output, "version", manifest.version);
  WritePathField(output, "session_dir", manifest.session_dir);
  WritePathField(output, "packet_file_path", manifest.packet_file_path);
  WriteField(output, "video_track_id", video.track_id);
  WriteField(output, "video_codec", L"h264");
  WriteField(output, "h264_packet_format", video.h264_packet_format);
  WriteField(output, "h264_sps", BytesToHex(video.h264_sps));
  WriteField(output, "h264_pps", BytesToHex(video.h264_pps));
  WriteField(output, "h264_avcc_extradata",
             BytesToHex(video.h264_avcc_extradata));
  WriteField(output, "requested_width", video.requested_width);
  WriteField(output, "requested_height", video.requested_height);
  WriteField(output, "requested_fps_numerator",
             video.requested_fps_numerator);
  WriteField(output, "requested_fps_denominator",
             video.requested_fps_denominator);
  WriteField(output, "requested_bitrate_bps", video.requested_bitrate_bps);
  WriteField(output, "requested_gop_seconds", video.requested_gop_seconds);
  WriteField(output, "requested_max_b_frames", video.requested_max_b_frames);
  WriteField(output, "encoder_name", video.encoder_name);
  WriteField(output, "encoder_clsid", video.encoder_clsid);
  WriteField(output, "encoder_enumeration_flags",
             video.encoder_enumeration_flags);
  WriteField(output, "media_width", video.media_width);
  WriteField(output, "media_height", video.media_height);
  WriteField(output, "media_fps_numerator", video.media_fps_numerator);
  WriteField(output, "media_fps_denominator", video.media_fps_denominator);
  WriteField(output, "media_bitrate_bps", video.media_bitrate_bps);
  WriteField(output, "media_gop_frame_count", video.media_gop_frame_count);
  WriteField(output, "media_h264_profile", video.media_h264_profile);
  WriteField(output, "media_max_b_frames", video.media_max_b_frames);
  WriteField(output, "d3d11_aware", video.d3d11_aware);
  WriteField(output, "device_manager_attached",
             video.device_manager_attached);
  WriteField(output, "async_transform", video.async_transform);
  WriteField(output, "async_unlocked", video.async_unlocked);
  WriteField(output, "codec_api_available", video.codec_api_available);
  WriteField(output, "monitor_device_name", video.monitor_device_name);
  WriteField(output, "monitor_primary", video.monitor_primary);
  WriteField(output, "monitor_left", video.monitor_left);
  WriteField(output, "monitor_top", video.monitor_top);
  WriteField(output, "monitor_right", video.monitor_right);
  WriteField(output, "monitor_bottom", video.monitor_bottom);
  WriteField(output, "source_width", video.source_width);
  WriteField(output, "source_height", video.source_height);
  WriteField(output, "output_width", video.output_width);
  WriteField(output, "output_height", video.output_height);
  WriteField(output, "audio_track_count",
              static_cast<uint32_t>(manifest.audio_tracks.size()));
  for (size_t index = 0; index < manifest.audio_tracks.size(); ++index) {
    const auto& audio = manifest.audio_tracks[index];
    WriteField(output, AudioKey(index, "track_id").c_str(), audio.track_id);
    WriteField(output, AudioKey(index, "codec").c_str(), L"aac");
    WriteField(output, AudioKey(index, "source_kind").c_str(),
               audio.source_kind);
    WriteField(output, AudioKey(index, "source_index").c_str(),
               audio.source_index);
    WriteField(output, AudioKey(index, "name").c_str(), audio.name);
    WriteField(output, AudioKey(index, "sample_rate").c_str(),
               audio.sample_rate);
    WriteField(output, AudioKey(index, "channel_count").c_str(),
               static_cast<uint32_t>(audio.channel_count));
    WriteField(output, AudioKey(index, "bitrate_bps").c_str(),
               audio.bitrate_bps);
    WriteField(output, AudioKey(index, "aac_frame_samples").c_str(),
               audio.aac_frame_samples);
    WriteField(output, AudioKey(index, "aac_payload_type").c_str(),
               audio.aac_payload_type);
    WriteField(output,
               AudioKey(index, "aac_profile_level_indication").c_str(),
               audio.aac_profile_level_indication);
    WriteField(output, AudioKey(index, "aac_audio_object_type").c_str(),
               audio.aac_audio_object_type);
    WriteField(output,
               AudioKey(index, "aac_audio_specific_config").c_str(),
               BytesToHex(audio.aac_audio_specific_config));
    WriteField(output, AudioKey(index, "encoder_name").c_str(),
               audio.encoder_name);
  }
  WriteField(output, "bookmark_count",
             static_cast<uint32_t>(manifest.bookmarks.size()),
             manifest.bookmarks.empty());
  for (size_t index = 0; index < manifest.bookmarks.size(); ++index) {
    const auto& bookmark = manifest.bookmarks[index];
    const bool is_last = index + 1u == manifest.bookmarks.size();
    WriteField(output, BookmarkKey(index, "id").c_str(), bookmark.id);
    WriteField(output, BookmarkKey(index, "time_ns").c_str(),
               bookmark.time_ns);
    WriteField(output, BookmarkKey(index, "label").c_str(), bookmark.label);
    WriteField(output, BookmarkKey(index, "default_pre_ns").c_str(),
               bookmark.default_pre_ns);
    WriteField(output, BookmarkKey(index, "default_post_ns").c_str(),
               bookmark.default_post_ns);
    WriteField(output, BookmarkKey(index, "user_note").c_str(),
               bookmark.user_note, is_last);
  }
  output << "}\n";
  if (!output) {
    const auto stream_error = StreamErrorCode();
    output.close();
    return TemporaryWriteFailure(
        temp_path, DiskWriteOperation::WriteContents, stream_error,
        L"The temporary manifest was incomplete and was not published.");
  }

  errno = 0;
  output.flush();
  if (!output) {
    const auto stream_error = StreamErrorCode();
    output.close();
    return TemporaryWriteFailure(
        temp_path, DiskWriteOperation::Flush, stream_error,
        L"The temporary manifest could not be made durable.");
  }

  errno = 0;
  output.close();
  if (!output) {
    return TemporaryWriteFailure(
        temp_path, DiskWriteOperation::Close, StreamErrorCode(),
        L"The temporary manifest could not be closed cleanly.");
  }

  DiskWriteFault publish_fault;
  if (!AtomicPublishFile(temp_path, path, true,
                         DiskWriteSubsystem::SessionManifest,
                         &publish_fault)) {
    std::wstring message = DescribeDiskWriteFault(publish_fault);
    message += L" The complete temporary manifest remains at '" +
               temp_path.wstring() + L"'.";
    return Result(SessionManifestStatus::WriteFailed, std::move(message),
                   std::move(publish_fault));
  }

  return Result(SessionManifestStatus::Success, L"");
}

SessionManifestResult ReadSessionManifest(
    const std::filesystem::path& session_dir,
    SessionManifest* manifest) {
  if (session_dir.empty()) {
    return Result(SessionManifestStatus::InvalidConfig,
                  L"Session manifest read needs a session directory and "
                  L"output destination.");
  }

  return ReadSessionManifestFile(SessionManifestPath(session_dir), manifest);
}

SessionManifestResult ReadSessionManifestFile(
    const std::filesystem::path& manifest_file_path,
    SessionManifest* manifest) {
  if (manifest == nullptr || manifest_file_path.empty()) {
    return Result(SessionManifestStatus::InvalidConfig,
                  L"Session manifest read needs a file path and output "
                  L"destination.");
  }

  *manifest = {};

  std::ifstream input(manifest_file_path, std::ios::binary);
  if (!input.is_open()) {
    return Result(SessionManifestStatus::ReadFailed,
                  L"Could not open session manifest for reading.");
  }

  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  SessionManifest parsed;
  parsed.session_dir = manifest_file_path.parent_path();

  if (!ReadUint32(json, "version", &parsed.version)) {
    return Result(SessionManifestStatus::ParseFailed,
                  L"Session manifest is missing a version.");
  }
  if (parsed.version != kLegacyVideoOnlySessionManifestVersion &&
      parsed.version != kAudioSessionManifestVersion &&
      parsed.version != kSessionManifestVersion) {
    return Result(SessionManifestStatus::UnsupportedVersion,
                  L"Session manifest version is not supported.");
  }

  std::wstring session_dir_text;
  std::wstring packet_file_text;
  if (!ReadString(json, "session_dir", &session_dir_text) ||
      !ReadString(json, "packet_file_path", &packet_file_text)) {
    return Result(SessionManifestStatus::ParseFailed,
                  L"Session manifest is missing paths.");
  }
  parsed.session_dir = std::filesystem::path(session_dir_text);
  parsed.packet_file_path = std::filesystem::path(packet_file_text);

  const auto stored_packet_path = parsed.session_dir / L"packets.dat";
  const auto actual_session_dir = manifest_file_path.parent_path();
  const auto actual_packet_path = actual_session_dir / L"packets.dat";
  std::error_code packet_error;
  if (parsed.packet_file_path.lexically_normal() ==
          stored_packet_path.lexically_normal() &&
      parsed.session_dir.lexically_normal() !=
          actual_session_dir.lexically_normal() &&
      std::filesystem::is_regular_file(actual_packet_path, packet_error) &&
      !packet_error) {
    parsed.session_dir = actual_session_dir;
    parsed.packet_file_path = actual_packet_path;
  }

  auto& video = parsed.video;
  if (!ReadUint32(json, "video_track_id", &video.track_id) ||
      !ReadCodec(json, "video_codec", &video.codec_id) ||
      !ReadString(json, "h264_packet_format", &video.h264_packet_format) ||
      !ReadBytes(json, "h264_sps", &video.h264_sps) ||
      !ReadBytes(json, "h264_pps", &video.h264_pps) ||
      !ReadBytes(json, "h264_avcc_extradata",
                 &video.h264_avcc_extradata) ||
      !ReadUint32(json, "requested_width", &video.requested_width) ||
      !ReadUint32(json, "requested_height", &video.requested_height) ||
      !ReadUint32(json, "requested_fps_numerator",
                  &video.requested_fps_numerator) ||
      !ReadUint32(json, "requested_fps_denominator",
                  &video.requested_fps_denominator) ||
      !ReadUint32(json, "requested_bitrate_bps",
                  &video.requested_bitrate_bps) ||
      !ReadDouble(json, "requested_gop_seconds",
                  &video.requested_gop_seconds) ||
      !ReadUint32(json, "requested_max_b_frames",
                  &video.requested_max_b_frames) ||
      !ReadString(json, "encoder_name", &video.encoder_name) ||
      !ReadString(json, "encoder_clsid", &video.encoder_clsid) ||
      !ReadUint32(json, "encoder_enumeration_flags",
                  &video.encoder_enumeration_flags) ||
      !ReadUint32(json, "media_width", &video.media_width) ||
      !ReadUint32(json, "media_height", &video.media_height) ||
      !ReadUint32(json, "media_fps_numerator",
                  &video.media_fps_numerator) ||
      !ReadUint32(json, "media_fps_denominator",
                  &video.media_fps_denominator) ||
      !ReadUint32(json, "media_bitrate_bps", &video.media_bitrate_bps) ||
      !ReadUint32(json, "media_gop_frame_count",
                  &video.media_gop_frame_count) ||
      !ReadUint32(json, "media_h264_profile", &video.media_h264_profile) ||
      !ReadUint32(json, "media_max_b_frames", &video.media_max_b_frames) ||
      !ReadBool(json, "d3d11_aware", &video.d3d11_aware) ||
      !ReadBool(json, "device_manager_attached",
                &video.device_manager_attached) ||
      !ReadBool(json, "async_transform", &video.async_transform) ||
      !ReadBool(json, "async_unlocked", &video.async_unlocked) ||
      !ReadBool(json, "codec_api_available", &video.codec_api_available) ||
      !ReadString(json, "monitor_device_name", &video.monitor_device_name) ||
      !ReadBool(json, "monitor_primary", &video.monitor_primary) ||
      !ReadInt32(json, "monitor_left", &video.monitor_left) ||
      !ReadInt32(json, "monitor_top", &video.monitor_top) ||
      !ReadInt32(json, "monitor_right", &video.monitor_right) ||
      !ReadInt32(json, "monitor_bottom", &video.monitor_bottom) ||
      !ReadUint32(json, "source_width", &video.source_width) ||
      !ReadUint32(json, "source_height", &video.source_height) ||
      !ReadUint32(json, "output_width", &video.output_width) ||
      !ReadUint32(json, "output_height", &video.output_height)) {
    return Result(SessionManifestStatus::ParseFailed,
                  L"Session manifest is missing required video fields.");
  }

  if (parsed.version != kLegacyVideoOnlySessionManifestVersion) {
    uint32_t audio_track_count = 0;
    if (!ReadUint32(json, "audio_track_count", &audio_track_count)) {
      return Result(SessionManifestStatus::ParseFailed,
                    L"Session manifest is missing the audio track count.");
    }

    parsed.audio_tracks.reserve(audio_track_count);
    for (uint32_t index = 0; index < audio_track_count; ++index) {
      AudioTrackSessionManifest audio;
      uint32_t channel_count = 0;
      if (!ReadUint32(json, AudioKey(index, "track_id"), &audio.track_id) ||
          !ReadCodec(json, AudioKey(index, "codec"), &audio.codec_id) ||
          !ReadString(json, AudioKey(index, "source_kind"),
                      &audio.source_kind) ||
          !ReadUint32(json, AudioKey(index, "source_index"),
                      &audio.source_index) ||
          !ReadString(json, AudioKey(index, "name"), &audio.name) ||
          !ReadUint32(json, AudioKey(index, "sample_rate"),
                      &audio.sample_rate) ||
          !ReadUint32(json, AudioKey(index, "channel_count"),
                      &channel_count) ||
          channel_count > std::numeric_limits<uint16_t>::max() ||
          !ReadUint32(json, AudioKey(index, "bitrate_bps"),
                      &audio.bitrate_bps) ||
          !ReadUint32(json, AudioKey(index, "aac_frame_samples"),
                      &audio.aac_frame_samples) ||
          !ReadUint32(json, AudioKey(index, "aac_payload_type"),
                      &audio.aac_payload_type) ||
          !ReadUint32(json,
                      AudioKey(index, "aac_profile_level_indication"),
                      &audio.aac_profile_level_indication) ||
          !ReadUint32(json, AudioKey(index, "aac_audio_object_type"),
                      &audio.aac_audio_object_type) ||
          !ReadBytes(json, AudioKey(index, "aac_audio_specific_config"),
                     &audio.aac_audio_specific_config) ||
          !ReadString(json, AudioKey(index, "encoder_name"),
                      &audio.encoder_name)) {
        return Result(SessionManifestStatus::ParseFailed,
                      L"Session manifest is missing required AAC track "
                      L"fields.");
      }
      audio.channel_count = static_cast<uint16_t>(channel_count);
      parsed.audio_tracks.push_back(std::move(audio));
    }
  }

  if (parsed.version == kSessionManifestVersion) {
    uint32_t bookmark_count = 0;
    if (!ReadUint32(json, "bookmark_count", &bookmark_count)) {
      return Result(SessionManifestStatus::ParseFailed,
                    L"Session manifest is missing the bookmark count.");
    }

    parsed.bookmarks.reserve(bookmark_count);
    for (uint32_t index = 0; index < bookmark_count; ++index) {
      Bookmark bookmark;
      if (!ReadUint64(json, BookmarkKey(index, "id"), &bookmark.id) ||
          !ReadInt64(json, BookmarkKey(index, "time_ns"),
                     &bookmark.time_ns) ||
          !ReadString(json, BookmarkKey(index, "label"),
                      &bookmark.label) ||
          !ReadInt64(json, BookmarkKey(index, "default_pre_ns"),
                     &bookmark.default_pre_ns) ||
          !ReadInt64(json, BookmarkKey(index, "default_post_ns"),
                     &bookmark.default_post_ns) ||
          !ReadString(json, BookmarkKey(index, "user_note"),
                      &bookmark.user_note)) {
        return Result(SessionManifestStatus::ParseFailed,
                      L"Session manifest is missing required bookmark fields.");
      }
      parsed.bookmarks.push_back(std::move(bookmark));
    }
  }

  if (!parsed.IsReady()) {
    return Result(SessionManifestStatus::ParseFailed,
                  L"Session manifest metadata is incomplete.");
  }

  *manifest = std::move(parsed);
  return Result(SessionManifestStatus::Success, L"");
}

const wchar_t* SessionManifestStatusName(
    SessionManifestStatus status) noexcept {
  switch (status) {
    case SessionManifestStatus::Success:
      return L"success";
    case SessionManifestStatus::InvalidConfig:
      return L"invalid config";
    case SessionManifestStatus::WriteFailed:
      return L"write failed";
    case SessionManifestStatus::ReadFailed:
      return L"read failed";
    case SessionManifestStatus::ParseFailed:
      return L"parse failed";
    case SessionManifestStatus::UnsupportedVersion:
      return L"unsupported version";
  }

  return L"unknown";
}

}  // namespace olouie::record
