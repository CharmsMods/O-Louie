#include <windows.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr DWORD kDefaultHoldMilliseconds = 3000;
constexpr DWORD kMaximumHoldMilliseconds = 30000;

struct DisplayModeRestore final {
  std::wstring device_name;
  DEVMODEW original{};
  bool armed = false;

  ~DisplayModeRestore() {
    if (armed) {
      const LONG result = ChangeDisplaySettingsExW(
          device_name.c_str(), &original, nullptr, CDS_FULLSCREEN, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        std::wcerr << L"Automatic display-mode restore failed with code "
                   << result << L".\n";
      }
    }
  }
};

std::optional<DWORD> ParseHoldMilliseconds(int argc, wchar_t** argv) {
  if (argc == 1) {
    return kDefaultHoldMilliseconds;
  }
  if (argc != 2 || argv[1][0] == L'\0') {
    return std::nullopt;
  }
  wchar_t* end = nullptr;
  const unsigned long parsed = wcstoul(argv[1], &end, 10);
  if (end == argv[1] || *end != L'\0' || parsed == 0 ||
      parsed > kMaximumHoldMilliseconds) {
    return std::nullopt;
  }
  return static_cast<DWORD>(parsed);
}

bool IsCompatibleAlternate(const DEVMODEW& original,
                           const DEVMODEW& candidate) noexcept {
  return candidate.dmPelsWidth >= 640 && candidate.dmPelsHeight >= 480 &&
         (candidate.dmPelsWidth != original.dmPelsWidth ||
          candidate.dmPelsHeight != original.dmPelsHeight) &&
         candidate.dmBitsPerPel == original.dmBitsPerPel &&
         candidate.dmDisplayFrequency == original.dmDisplayFrequency;
}

std::optional<DEVMODEW> FindAlternateMode(
    const std::wstring& device_name,
    const DEVMODEW& original) {
  std::optional<DEVMODEW> best;
  uint64_t best_area = 0;
  const uint64_t original_area =
      static_cast<uint64_t>(original.dmPelsWidth) * original.dmPelsHeight;

  for (DWORD index = 0;; ++index) {
    DEVMODEW candidate{};
    candidate.dmSize = sizeof(candidate);
    if (!EnumDisplaySettingsExW(device_name.c_str(), index, &candidate, 0)) {
      break;
    }
    if (!IsCompatibleAlternate(original, candidate)) {
      continue;
    }
    const uint64_t area =
        static_cast<uint64_t>(candidate.dmPelsWidth) * candidate.dmPelsHeight;
    if (area < original_area && area > best_area) {
      best = candidate;
      best_area = area;
    }
  }
  return best;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const auto hold_milliseconds = ParseHoldMilliseconds(argc, argv);
  if (!hold_milliseconds.has_value()) {
    std::wcerr << L"Usage: O'LouieDisplayModeExercise [hold-milliseconds: "
                  L"1-30000]\n";
    return 2;
  }

  const HMONITOR primary =
      MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (primary == nullptr || !GetMonitorInfoW(primary, &monitor_info)) {
    std::wcerr << L"Could not resolve the primary monitor.\n";
    return 1;
  }

  DEVMODEW original{};
  original.dmSize = sizeof(original);
  if (!EnumDisplaySettingsExW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS,
                              &original, 0)) {
    std::wcerr << L"Could not read the primary monitor display mode.\n";
    return 1;
  }

  const auto alternate = FindAlternateMode(monitor_info.szDevice, original);
  if (!alternate.has_value()) {
    std::wcerr << L"No same-format, same-refresh smaller display mode is "
                  L"available for a safe exercise.\n";
    return 3;
  }

  DisplayModeRestore restore{monitor_info.szDevice, original, false};
  DEVMODEW requested = *alternate;
  requested.dmFields =
      DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
  const LONG changed = ChangeDisplaySettingsExW(
      restore.device_name.c_str(), &requested, nullptr, CDS_FULLSCREEN,
      nullptr);
  if (changed != DISP_CHANGE_SUCCESSFUL) {
    std::wcerr << L"Temporary display-mode change failed with code "
               << changed << L".\n";
    return 1;
  }
  restore.armed = true;

  std::wcout << L"Changed " << restore.device_name << L" from "
             << original.dmPelsWidth << L"x" << original.dmPelsHeight
             << L" to " << requested.dmPelsWidth << L"x"
             << requested.dmPelsHeight << L" for " << *hold_milliseconds
             << L" ms.\n";
  std::this_thread::sleep_for(
      std::chrono::milliseconds(*hold_milliseconds));

  const LONG restored = ChangeDisplaySettingsExW(
      restore.device_name.c_str(), &restore.original, nullptr, CDS_FULLSCREEN,
      nullptr);
  if (restored != DISP_CHANGE_SUCCESSFUL) {
    std::wcerr << L"Display-mode restore failed with code " << restored
               << L".\n";
    return 1;
  }
  restore.armed = false;
  std::wcout << L"Restored " << restore.device_name << L" to "
             << original.dmPelsWidth << L"x" << original.dmPelsHeight
             << L".\n";
  return 0;
}
