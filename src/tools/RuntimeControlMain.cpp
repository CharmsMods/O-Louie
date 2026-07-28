#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kHiddenWindowClass[] = L"O'Louie.HiddenWindow";
constexpr wchar_t kSettingsWindowClass[] = L"O'Louie.ImGuiDx11Settings";
constexpr UINT_PTR kToggleRecordingCommand = 1000;
constexpr UINT_PTR kOpenSettingsCommand = 1001;
constexpr UINT_PTR kExitCommand = 1002;
constexpr UINT_PTR kFirstClipCommand = 1003;
constexpr UINT_PTR kThirdClipCommand = 1004;
constexpr UINT_PTR kBookmarkCommand = 1005;
constexpr UINT_PTR kSecondClipCommand = 1006;
constexpr UINT_PTR kCustomClipCommand = 1007;

HWND WaitForWindow(std::wstring_view class_name,
                   std::chrono::milliseconds timeout,
                   bool require_visible = false) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    HWND window = FindWindowW(std::wstring(class_name).c_str(), nullptr);
    if (window != nullptr && (!require_visible || IsWindowVisible(window))) {
      return window;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  } while (std::chrono::steady_clock::now() < deadline);
  return nullptr;
}

bool PostTrayCommand(UINT_PTR command) {
  const HWND hidden = WaitForWindow(kHiddenWindowClass,
                                    std::chrono::seconds(5));
  return hidden != nullptr &&
         PostMessageW(hidden, WM_COMMAND, MAKEWPARAM(command, 0), 0) != FALSE;
}

LPARAM ClientPoint(HWND window, double x_fraction, double y_fraction) {
  RECT client{};
  GetClientRect(window, &client);
  const LONG client_width = client.right - client.left;
  const LONG client_height = client.bottom - client.top;
  const int width = static_cast<int>(client_width > 0 ? client_width : 1);
  const int height = static_cast<int>(client_height > 0 ? client_height : 1);
  const int x = static_cast<int>(std::lround(width * x_fraction));
  const int y = static_cast<int>(std::lround(height * y_fraction));
  return MAKELPARAM(x, y);
}

void ClickClient(HWND window, double x_fraction, double y_fraction) {
  const LPARAM point = ClientPoint(window, x_fraction, y_fraction);
  SendMessageW(window, WM_MOUSEMOVE, 0, point);
  SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
  SendMessageW(window, WM_LBUTTONUP, 0, point);
}

bool FocusWindow(HWND window) {
  const DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
  const DWORD current_thread = GetCurrentThreadId();
  const bool attached = target_thread != current_thread &&
                        AttachThreadInput(current_thread, target_thread, TRUE);
  ShowWindow(window, SW_RESTORE);
  const bool foreground = SetForegroundWindow(window) != FALSE;
  SetFocus(window);
  if (attached) {
    AttachThreadInput(current_thread, target_thread, FALSE);
  }
  return foreground;
}

bool ReplaceActiveText(HWND window, std::wstring_view text) {
  FocusWindow(window);
  INPUT select_all[4]{};
  select_all[0].type = INPUT_KEYBOARD;
  select_all[0].ki.wVk = VK_CONTROL;
  select_all[1].type = INPUT_KEYBOARD;
  select_all[1].ki.wVk = 'A';
  select_all[2].type = INPUT_KEYBOARD;
  select_all[2].ki.wVk = 'A';
  select_all[2].ki.dwFlags = KEYEVENTF_KEYUP;
  select_all[3].type = INPUT_KEYBOARD;
  select_all[3].ki.wVk = VK_CONTROL;
  select_all[3].ki.dwFlags = KEYEVENTF_KEYUP;
  if (SendInput(static_cast<UINT>(std::size(select_all)), select_all,
                sizeof(INPUT)) != std::size(select_all)) {
    return false;
  }

  std::vector<INPUT> characters;
  characters.reserve(text.size() * 2);
  for (const wchar_t character : text) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = character;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    characters.push_back(down);
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    characters.push_back(up);
  }
  return characters.empty() ||
         SendInput(static_cast<UINT>(characters.size()), characters.data(),
                   sizeof(INPUT)) == characters.size();
}

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::string JsonEscape(std::string_view text) {
  std::string result;
  result.reserve(text.size() * 2);
  for (const char character : text) {
    if (character == '\\' || character == '"') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  return result;
}

std::filesystem::path SettingsFilePath() {
  wchar_t local_app_data[32768]{};
  const DWORD length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data,
      static_cast<DWORD>(std::size(local_app_data)));
  if (length == 0 || length >= std::size(local_app_data)) {
    return {};
  }
  return std::filesystem::path(local_app_data) / L"O'Louie" / L"settings" /
         L"settings.json";
}

bool SettingsFileContainsOutput(const std::filesystem::path& output_path) {
  const auto settings_file = SettingsFilePath();
  std::ifstream input(settings_file, std::ios::binary);
  if (!input) {
    return false;
  }
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::string expected =
      "\"output_directory\":\"" +
      JsonEscape(ToUtf8(output_path.native())) + "\"";
  return json.find(expected) != std::string::npos;
}

bool SetOutputThroughUi(const std::filesystem::path& output_path) {
  if (!PostTrayCommand(kOpenSettingsCommand)) {
    std::wcerr << L"Could not post the Open Settings tray command.\n";
    return false;
  }
  HWND settings = WaitForWindow(kSettingsWindowClass,
                                std::chrono::seconds(5), true);
  if (settings == nullptr) {
    std::wcerr << L"The settings window did not become visible.\n";
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ClickClient(settings, 0.07, 0.10);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ClickClient(settings, 0.22, 0.235);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!ReplaceActiveText(settings, output_path.native())) {
    std::wcerr << L"Could not send the output path to the settings window.\n";
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ClickClient(settings, 0.085, 0.938);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         !SettingsFileContainsOutput(output_path)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!SettingsFileContainsOutput(output_path)) {
    std::wcerr << L"The settings UI did not persist the requested output path.\n";
    return false;
  }

  SendMessageW(settings, WM_CLOSE, 0, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  if (!PostTrayCommand(kOpenSettingsCommand)) {
    std::wcerr << L"Could not reopen settings through the tray command.\n";
    return false;
  }
  settings = WaitForWindow(kSettingsWindowClass,
                           std::chrono::seconds(5), true);
  if (settings == nullptr) {
    std::wcerr << L"The saved settings window did not reopen.\n";
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  SendMessageW(settings, WM_CLOSE, 0, 0);
  std::wcout << L"Settings output path was saved and the real window reopened: "
             << output_path.wstring() << L'\n';
  return true;
}

bool OpenDiagnostics() {
  if (!PostTrayCommand(kOpenSettingsCommand)) {
    std::wcerr << L"Could not post the Open Settings tray command.\n";
    return false;
  }
  const HWND settings = WaitForWindow(kSettingsWindowClass,
                                      std::chrono::seconds(5), true);
  if (settings == nullptr) {
    std::wcerr << L"The settings window did not become visible.\n";
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  FocusWindow(settings);
  ClickClient(settings, 0.59, 0.10);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::wcout << L"Opened the live Diagnostics tab.\n";
  return true;
}

UINT_PTR CommandForName(std::wstring_view name) {
  if (name == L"toggle-recording") {
    return kToggleRecordingCommand;
  }
  if (name == L"first-clip") {
    return kFirstClipCommand;
  }
  if (name == L"second-clip") {
    return kSecondClipCommand;
  }
  if (name == L"third-clip") {
    return kThirdClipCommand;
  }
  if (name == L"custom-clip") {
    return kCustomClipCommand;
  }
  if (name == L"bookmark") {
    return kBookmarkCommand;
  }
  if (name == L"open-settings") {
    return kOpenSettingsCommand;
  }
  if (name == L"exit") {
    return kExitCommand;
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::wcerr << L"Usage: O'LouieRuntimeControl <command|set-output> [path]\n";
    return 2;
  }

  const std::wstring_view command(argv[1]);
  if (command == L"set-output") {
    if (argc != 3 || argv[2][0] == L'\0') {
      std::wcerr << L"set-output requires a nonempty output directory.\n";
      return 2;
    }
    return SetOutputThroughUi(std::filesystem::path(argv[2])) ? 0 : 1;
  }
  if (command == L"open-diagnostics") {
    return OpenDiagnostics() ? 0 : 1;
  }

  const UINT_PTR command_id = CommandForName(command);
  if (command_id == 0) {
    std::wcerr << L"Unknown runtime command: " << command << L'\n';
    return 2;
  }
  if (!PostTrayCommand(command_id)) {
    std::wcerr << L"Could not post runtime command: " << command << L'\n';
    return 1;
  }
  std::wcout << L"Posted runtime command: " << command << L'\n';
  return 0;
}
