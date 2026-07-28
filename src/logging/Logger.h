#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace olouie::logging {

class Logger final {
 public:
  Logger() = default;
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  bool Open(const std::filesystem::path& log_path);
  void Close();

  void Info(std::wstring_view message);
  void Warning(std::wstring_view message);
  void Error(std::wstring_view message);

 private:
  enum class Level {
    Info,
    Warning,
    Error,
  };

  void Write(Level level, std::wstring_view message);

  std::mutex mutex_;
  std::ofstream stream_;
};

}  // namespace olouie::logging
