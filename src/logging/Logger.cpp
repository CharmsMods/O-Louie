#include "logging/Logger.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace olouie::logging {
namespace {

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  const int byte_count =
      WideCharToMultiByte(CP_UTF8, 0, value.data(),
                          static_cast<int>(value.size()), nullptr, 0, nullptr,
                          nullptr);
  if (byte_count <= 0) {
    return {};
  }

  std::string result(static_cast<size_t>(byte_count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), byte_count, nullptr, nullptr);
  return result;
}

std::string Timestamp() {
  SYSTEMTIME now{};
  GetLocalTime(&now);

  char buffer[40]{};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds);
  return buffer;
}

}  // namespace

Logger::~Logger() {
  Close();
}

bool Logger::Open(const std::filesystem::path& log_path) {
  std::lock_guard lock(mutex_);
  std::filesystem::create_directories(log_path.parent_path());
  stream_.open(log_path, std::ios::out | std::ios::app | std::ios::binary);
  return stream_.is_open();
}

void Logger::Close() {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) {
    stream_.flush();
    stream_.close();
  }
}

void Logger::Info(std::wstring_view message) {
  Write(Level::Info, message);
}

void Logger::Warning(std::wstring_view message) {
  Write(Level::Warning, message);
}

void Logger::Error(std::wstring_view message) {
  Write(Level::Error, message);
}

void Logger::Write(Level level, std::wstring_view message) {
  std::lock_guard lock(mutex_);
  if (!stream_.is_open()) {
    return;
  }

  const char* level_text = "unknown";
  switch (level) {
    case Level::Info:
      level_text = "info";
      break;
    case Level::Warning:
      level_text = "warning";
      break;
    case Level::Error:
      level_text = "error";
      break;
  }

  stream_ << Timestamp() << " [" << level_text << "] " << WideToUtf8(message)
          << '\n';
  stream_.flush();
}

}  // namespace olouie::logging
