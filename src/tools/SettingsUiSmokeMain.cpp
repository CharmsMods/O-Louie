#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "diagnostics/DiagnosticsSnapshot.h"
#include "logging/RuntimePaths.h"
#include "settings/Settings.h"
#include "ui/SettingsUi.h"
#include "win32/WindowAffinity.h"

namespace {

bool CaptureWindowBitmap(HWND window, const std::filesystem::path& path,
                         std::wstring* error) {
  RECT rect{};
  if (window == nullptr || !GetWindowRect(window, &rect)) {
    if (error != nullptr) {
      *error = L"Could not read the settings window bounds.";
    }
    return false;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  if (screen == nullptr || memory == nullptr || bitmap == nullptr) {
    if (bitmap != nullptr) {
      DeleteObject(bitmap);
    }
    if (memory != nullptr) {
      DeleteDC(memory);
    }
    if (screen != nullptr) {
      ReleaseDC(nullptr, screen);
    }
    if (error != nullptr) {
      *error = L"Could not allocate the settings screenshot surface.";
    }
    return false;
  }

  HGDIOBJ previous = SelectObject(memory, bitmap);
  const bool copied =
      PrintWindow(window, memory, PW_RENDERFULLCONTENT) != FALSE ||
      BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top,
             SRCCOPY | CAPTUREBLT) != FALSE;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) *
                              static_cast<size_t>(height) * 4u);
  const bool read = copied &&
                    GetDIBits(memory, bitmap, 0, static_cast<UINT>(height),
                              pixels.data(), &info, DIB_RGB_COLORS) != 0;

  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (!read) {
    if (error != nullptr) {
      *error = L"Could not capture the settings window pixels.";
    }
    return false;
  }

  BITMAPFILEHEADER file_header{};
  file_header.bfType = 0x4D42;
  file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits +
                       static_cast<DWORD>(pixels.size());

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(&file_header),
               sizeof(file_header));
  output.write(reinterpret_cast<const char*>(&info.bmiHeader),
               sizeof(info.bmiHeader));
  output.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
  if (!output) {
    if (error != nullptr) {
      *error = L"Could not write the settings screenshot.";
    }
    return false;
  }
  return true;
}

void PumpMessages(std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::wcerr << L"COM initialization failed.\n";
    return 1;
  }

  const auto output =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::current_path() / L"settings-ui-smoke.bmp";
  const auto paths = olouie::logging::RuntimePaths::FromRoot(
      std::filesystem::temp_directory_path() / L"O'LouieSettingsUiSmoke");
  paths.EnsureCreated();
  const auto defaults = olouie::settings::AppSettings::Defaults(paths);

  std::wstring diagnostic;
  olouie::audio::MicMonitorSnapshot mic_monitor;
  mic_monitor.generation = 3;
  mic_monitor.state = olouie::audio::MicMonitorState::Monitoring;
  mic_monitor.active_output_device_name = L"Smoke Test Headphones";
  mic_monitor.using_fallback_output = true;
  mic_monitor.peak_dbfs = -12.5f;
  mic_monitor.queue_capacity_frames = 12000;
  mic_monitor.queued_frame_count = 2200;
  mic_monitor.message =
      L"The saved output is unavailable; using Smoke Test Headphones.";
  olouie::ui::SettingsUi settings_ui;
  settings_ui.Configure(
      GetModuleHandleW(nullptr),
      [](const olouie::settings::AppSettings&, std::wstring*) { return true; },
      [&diagnostic](std::wstring_view message) {
        diagnostic = std::wstring(message);
      },
      [](const olouie::audio::MicMonitorOptions&) {
        return olouie::audio::MicMonitorCommandResult{
            olouie::audio::MicMonitorCommandStatus::Accepted,
            L"Microphone check started."};
      },
      [] {
        return olouie::audio::MicMonitorCommandResult{
            olouie::audio::MicMonitorCommandStatus::Accepted,
            L"Microphone check stopped."};
      },
      [&mic_monitor] { return mic_monitor; });
  olouie::diagnostics::DiagnosticsSnapshot diagnostics;
  diagnostics.generation = 17;
  diagnostics.recorder_state = olouie::record::VideoRecorderState::Idle;
  diagnostics.recovery_state =
      olouie::record::RecordingRecoveryState::Ready;
  diagnostics.monitor_identity = L"DISPLAY1 (Primary)";
  diagnostics.encoder_identity = L"Hardware H.264 Smoke MFT";
  diagnostics.requested_fps = 60.0;
  diagnostics.negotiated_fps = 60.0;
  diagnostics.observed_fps = 59.82;
  diagnostics.requested_bitrate_bps = 20000000;
  diagnostics.negotiated_bitrate_bps = 20000000;
  diagnostics.observed_bitrate_bps = 19600000;
  diagnostics.recorder_stats.captured_frame_count = 868;
  diagnostics.recorder_stats.accepted_frame_count = 716;
  diagnostics.recorder_stats.rate_limited_frame_count = 154;
  diagnostics.recorder_stats.dropped_frame_count = 2;
  diagnostics.recorder_stats.encoded_frame_count = 714;
  diagnostics.recorder_stats.encoded_packet_count = 714;
  diagnostics.recorder_stats.encoded_audio_packet_count = 560;
  diagnostics.recorder_stats.runtime.queued_video_frame_count = 2;
  diagnostics.recorder_stats.runtime.peak_queued_video_frame_count = 8;
  diagnostics.recorder_stats.runtime.video_queue_capacity = 8;
  diagnostics.recorder_stats.runtime.video_queue_oldest_frame_age_ns =
      33000000;
  diagnostics.recorder_stats.runtime.video_queue_maximum_frame_age_ns =
      133000000;
  diagnostics.recorder_stats.runtime.video_queue_overflow_event_count = 1;
  diagnostics.recorder_stats.runtime.video_queue_backlog_recovery_count = 1;
  diagnostics.recorder_stats.runtime.video_queue_dropped_backlog_count = 8;
  diagnostics.recorder_stats.runtime.video_queue_last_overflow_reason =
      olouie::capture::VideoFrameQueueOverflowReason::BacklogDiscarded;
  auto& texture_pool =
      diagnostics.recorder_stats.runtime.video_texture_pool;
  texture_pool.capacity = 10;
  texture_pool.allocated_texture_count = 10;
  texture_pool.in_use_texture_count = 3;
  texture_pool.peak_in_use_texture_count = 10;
  texture_pool.created_texture_count = 10;
  texture_pool.reused_texture_count = 704;
  diagnostics.recorder_stats.runtime.video_texture_pool_exhausted_frame_count =
      2;
  auto& converter = diagnostics.recorder_stats.runtime.video_converter;
  converter.input_view_create_count = 10;
  converter.input_view_reuse_count = 704;
  converter.output_view_create_count = 1;
  converter.output_view_reuse_count = 713;
  diagnostics.recorder_stats.runtime.outstanding_export_count = 1;
  diagnostics.audio_tracks = {{2, L"System audio", true},
                              {3, L"Microphone", true}};
  diagnostics.session_directory = paths.sessions / L"recording-active";
  diagnostics.recording_output_path = paths.exports / L"O'Louie-active.mp4";
  diagnostics.configured_output_directory = paths.exports;
  diagnostics.latest_export_state = L"clip queued";
  diagnostics.latest_export_path = paths.exports / L"O'Louie-clip-1.mp4";
  settings_ui.SetDiagnosticsSnapshot(std::move(diagnostics));
  std::wstring error;
  if (!settings_ui.Open(defaults, defaults, &error,
                        olouie::ui::SettingsUiInitialTab::Audio)) {
    std::wcerr << L"Settings UI open failed: " << error << L'\n';
    CoUninitialize();
    return 1;
  }

  PumpMessages(std::chrono::milliseconds(350));
  DWORD affinity = WDA_NONE;
  const bool exclusion_was_active =
      GetWindowDisplayAffinity(settings_ui.hwnd(), &affinity) != FALSE &&
      affinity == 0x00000011;
  if (!SetWindowDisplayAffinity(settings_ui.hwnd(), WDA_NONE)) {
    std::wcerr << L"Could not temporarily clear capture exclusion for the visual artifact.\n";
    settings_ui.Shutdown();
    CoUninitialize();
    return 1;
  }
  PumpMessages(std::chrono::milliseconds(100));
  if (!CaptureWindowBitmap(settings_ui.hwnd(), output, &error)) {
    std::wcerr << L"Settings UI capture failed: " << error << L'\n';
    settings_ui.Shutdown();
    CoUninitialize();
    return 1;
  }
  std::wstring restore_error;
  if (!olouie::win32::TryExcludeFromCapture(settings_ui.hwnd(),
                                             &restore_error)) {
    std::wcerr << L"Could not restore capture exclusion: " << restore_error
               << L'\n';
    settings_ui.Shutdown();
    CoUninitialize();
    return 1;
  }

  const auto visible_stats = settings_ui.host_stats();
  settings_ui.Close();
  const uint64_t hidden_frames =
      settings_ui.host_stats().render.rendered_frame_count;
  PumpMessages(std::chrono::milliseconds(150));
  const auto hidden_stats = settings_ui.host_stats();
  if (!settings_ui.Open(defaults, defaults, &error,
                        olouie::ui::SettingsUiInitialTab::Diagnostics)) {
    std::wcerr << L"Settings UI reopen failed: " << error << L'\n';
    settings_ui.Shutdown();
    CoUninitialize();
    return 1;
  }
  PumpMessages(std::chrono::milliseconds(100));
  const auto reopened_stats = settings_ui.host_stats();
  settings_ui.Close();
  const uint64_t second_hidden_frames =
      settings_ui.host_stats().render.rendered_frame_count;
  PumpMessages(std::chrono::milliseconds(100));
  const auto second_hidden_stats = settings_ui.host_stats();
  settings_ui.Shutdown();
  CoUninitialize();

  if (!exclusion_was_active || visible_stats.render.rendered_frame_count == 0 ||
      hidden_stats.render.rendered_frame_count != hidden_frames) {
    std::wcerr << L"Settings render lifecycle evidence failed.\n";
    return 1;
  }
  if (reopened_stats.render.rendered_frame_count <=
          hidden_stats.render.rendered_frame_count ||
      second_hidden_stats.render.rendered_frame_count !=
          second_hidden_frames) {
    std::wcerr << L"Settings repeated-open lifecycle evidence failed.\n";
    return 1;
  }

  std::wcout << L"Settings UI smoke succeeded.\n"
             << L"  Screenshot: " << output.wstring() << L'\n'
             << L"  Visible frames: "
             << visible_stats.render.rendered_frame_count << L'\n'
             << L"  Hidden frames after wait: "
             << hidden_stats.render.rendered_frame_count << L'\n'
             << L"  Reopened frames: "
             << reopened_stats.render.rendered_frame_count << L'\n'
             << L"  Capture excluded: "
             << (visible_stats.capture_excluded ? L"yes" : L"no") << L'\n';
  if (!diagnostic.empty()) {
    std::wcout << L"  Diagnostic: " << diagnostic << L'\n';
  }
  return 0;
}
