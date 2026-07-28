#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"O'Louie.LongSessionStimulus";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT_PTR kExitTimer = 2;
constexpr UINT kFrameIntervalMilliseconds = 250;
constexpr uint32_t kDefaultDurationSeconds = 3900;
constexpr uint32_t kMaximumDurationSeconds = 7200;

std::chrono::steady_clock::time_point g_started;
uint64_t g_frame = 0;

uint32_t ParseDurationSeconds() {
  const wchar_t* command_line = GetCommandLineW();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(command_line, &argc);
  if (argv == nullptr) {
    return 0;
  }
  uint32_t result = kDefaultDurationSeconds;
  if (argc > 2) {
    result = 0;
  } else if (argc == 2) {
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(argv[1], &end, 10);
    if (end == argv[1] || *end != L'\0' || parsed == 0 ||
        parsed > kMaximumDurationSeconds) {
      result = 0;
    } else {
      result = static_cast<uint32_t>(parsed);
    }
  }
  LocalFree(argv);
  return result;
}

COLORREF FrameColor(uint64_t frame, uint8_t offset) {
  const uint8_t red = static_cast<uint8_t>(48 + (frame * 7 + offset) % 160);
  const uint8_t green =
      static_cast<uint8_t>(48 + (frame * 11 + offset * 3) % 160);
  const uint8_t blue =
      static_cast<uint8_t>(48 + (frame * 17 + offset * 5) % 160);
  return RGB(red, green, blue);
}

void Paint(HWND window) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);

  HBRUSH background = CreateSolidBrush(RGB(24, 27, 33));
  FillRect(dc, &client, background);
  DeleteObject(background);

  const int bar_width = std::max(1L, (client.right - client.left) / 6);
  RECT bar{client.left, client.top, client.left + bar_width, client.top + 12};
  for (uint8_t index = 0; index < 6; ++index) {
    bar.left = client.left + index * bar_width;
    bar.right = index == 5 ? client.right : bar.left + bar_width;
    HBRUSH brush = CreateSolidBrush(FrameColor(g_frame, index * 29));
    FillRect(dc, &bar, brush);
    DeleteObject(brush);
  }

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(241, 244, 248));
  HFONT font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HFONT old_font = static_cast<HFONT>(SelectObject(dc, font));

  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - g_started);
  const uint64_t hours = static_cast<uint64_t>(elapsed.count()) / 3600;
  const uint64_t minutes = (static_cast<uint64_t>(elapsed.count()) / 60) % 60;
  const uint64_t seconds = static_cast<uint64_t>(elapsed.count()) % 60;
  wchar_t text[128]{};
  swprintf_s(text, L"O'Louie long-session exercise  %02llu:%02llu:%02llu",
             hours, minutes, seconds);
  RECT text_rect{client.left + 14, client.top + 25, client.right - 14,
                 client.bottom - 8};
  DrawTextW(dc, text, -1, &text_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  SelectObject(dc, old_font);
  DeleteObject(font);
  EndPaint(window, &paint);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_TIMER:
      if (wparam == kFrameTimer) {
        ++g_frame;
        InvalidateRect(window, nullptr, FALSE);
      } else if (wparam == kExitTimer) {
        DestroyWindow(window);
      }
      return 0;
    case WM_PAINT:
      Paint(window);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kFrameTimer);
      KillTimer(window, kExitTimer);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const uint32_t duration_seconds = ParseDurationSeconds();
  if (duration_seconds == 0) {
    MessageBoxW(nullptr,
                L"Usage: O'LouieLongSessionStimulus [duration-seconds: 1-7200]",
                L"O'Louie long-session stimulus", MB_OK | MB_ICONERROR);
    return 2;
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    return 1;
  }

  constexpr int width = 520;
  constexpr int height = 104;
  const int screen_width = GetSystemMetrics(SM_CXSCREEN);
  const int screen_height = GetSystemMetrics(SM_CYSCREEN);
  HWND window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass,
      L"O'Louie long-session exercise", WS_POPUP | WS_BORDER,
      std::max(0, screen_width - width - 18),
      std::max(0, screen_height - height - 58), width, height, nullptr,
      nullptr, instance, nullptr);
  if (window == nullptr) {
    return 1;
  }

  g_started = std::chrono::steady_clock::now();
  ShowWindow(window, SW_SHOWNOACTIVATE);
  SetTimer(window, kFrameTimer, kFrameIntervalMilliseconds, nullptr);
  SetTimer(window, kExitTimer, duration_seconds * 1000, nullptr);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
