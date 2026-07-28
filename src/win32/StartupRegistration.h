#pragma once

#include <filesystem>
#include <string>

namespace olouie::win32 {

bool SetStartupRegistration(bool enabled,
                            const std::filesystem::path& executable_path,
                            std::wstring* error);
std::filesystem::path CurrentExecutablePath(std::wstring* error);

}  // namespace olouie::win32
