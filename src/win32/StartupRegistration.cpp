#include "win32/StartupRegistration.h"

#include <windows.h>

#include <array>
#include <string>

#include "product/ProductIdentity.h"

namespace olouie::win32 {
namespace {

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

LSTATUS DeleteValueIfPresent(HKEY key, const wchar_t* value_name) {
  const LSTATUS status = RegDeleteValueW(key, value_name);
  return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

}  // namespace

std::filesystem::path CurrentExecutablePath(std::wstring* error) {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    SetError(error, L"Could not resolve the O'Louie executable path.");
    return {};
  }
  path.resize(length);
  return std::filesystem::path(path);
}

bool SetStartupRegistration(bool enabled,
                            const std::filesystem::path& executable_path,
                            std::wstring* error) {
  if (enabled && executable_path.empty()) {
    SetError(error, L"Start-with-Windows needs a valid executable path.");
    return false;
  }

  HKEY raw_key = nullptr;
  const LSTATUS opened = RegCreateKeyExW(
      HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &raw_key, nullptr);
  if (opened != ERROR_SUCCESS) {
    SetError(error, L"Could not open the current-user startup registry key (Win32 error " +
                        std::to_wstring(opened) + L").");
    return false;
  }

  struct KeyCloser {
    HKEY key;
    ~KeyCloser() {
      if (key != nullptr) {
        RegCloseKey(key);
      }
    }
  } closer{raw_key};

  LSTATUS status = ERROR_SUCCESS;
  if (enabled) {
    const std::wstring command = L"\"" + executable_path.wstring() + L"\"";
    status = RegSetValueExW(
        raw_key, product::kDisplayName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS) {
      status =
          DeleteValueIfPresent(raw_key, product::kLegacyRuntimeFolderName);
    }
  } else {
    status = DeleteValueIfPresent(raw_key, product::kDisplayName);
    if (status == ERROR_SUCCESS) {
      status =
          DeleteValueIfPresent(raw_key, product::kLegacyRuntimeFolderName);
    }
  }
  if (status != ERROR_SUCCESS) {
    SetError(error,
             L"Could not update the current-user startup setting (Win32 error " +
                 std::to_wstring(status) + L").");
    return false;
  }
  return true;
}

}  // namespace olouie::win32
