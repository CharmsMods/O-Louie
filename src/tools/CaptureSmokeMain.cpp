#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "capture/WgcMonitorCapture.h"
#include "graphics/D3D11DeviceContext.h"
#include "graphics/DisplayManager.h"
#include "graphics/GpuBgraToNv12.h"

namespace {

void PrintUsage() {
  std::wcout << L"Usage:\n"
             << L"  O'LouieCaptureSmoke.exe\n"
             << L"  O'LouieCaptureSmoke.exe --wgc [duration_ms]\n";
}

bool ParseDuration(int argc, wchar_t** argv, std::chrono::milliseconds* value) {
  if (argc < 3) {
    *value = std::chrono::milliseconds(3000);
    return true;
  }

  try {
    const int duration = std::stoi(argv[2]);
    if (duration <= 0) {
      return false;
    }

    *value = std::chrono::milliseconds(duration);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool run_wgc = argc >= 2 && std::wstring(argv[1]) == L"--wgc";

  if (argc >= 2 && !run_wgc) {
    PrintUsage();
    return 2;
  }

  std::chrono::milliseconds duration(0);
  if (run_wgc && !ParseDuration(argc, argv, &duration)) {
    PrintUsage();
    return 2;
  }

  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  const auto monitors = olouie::graphics::EnumerateMonitors();
  std::wcout << L"Monitors: " << monitors.size() << L'\n';
  for (size_t index = 0; index < monitors.size(); ++index) {
    const auto& monitor = monitors[index];
    std::wcout << L"  [" << index << L"] " << monitor.device_name << L" "
               << monitor.Width() << L"x" << monitor.Height();
    if (monitor.primary) {
      std::wcout << L" primary";
    }
    std::wcout << L'\n';
  }

  const auto* monitor = olouie::graphics::FindPrimaryMonitor(monitors);
  if (monitor == nullptr) {
    std::wcerr << L"No monitor is available.\n";
    return 1;
  }

  std::wstring d3d_error;
  auto d3d =
      olouie::graphics::D3D11DeviceContext::CreateForMonitor(monitor->handle,
                                                             &d3d_error);
  if (!d3d.IsValid()) {
    std::wcerr << d3d_error << L'\n';
    return 1;
  }

  std::wcout << L"D3D11 adapter: " << d3d.adapter_description() << L'\n';
  const auto convert_smoke =
      olouie::graphics::CreateGpuBgraToNv12SmokeTextures(
          d3d.device(), {static_cast<uint32_t>(monitor->Width()),
                         static_cast<uint32_t>(monitor->Height()),
                         static_cast<uint32_t>(monitor->Width() & ~1),
                         static_cast<uint32_t>(monitor->Height() & ~1)});
  std::wcout << L"BGRA-to-NV12 texture smoke: "
             << olouie::graphics::GpuBgraToNv12StatusName(
                    convert_smoke.status)
             << L'\n';
  if (!convert_smoke.message.empty()) {
    std::wcout << L"  " << convert_smoke.message << L'\n';
  }
  if (convert_smoke.Succeeded()) {
    std::wcout << L"  source: " << convert_smoke.plan.source_width << L"x"
               << convert_smoke.plan.source_height << L" BGRA\n"
               << L"  output: " << convert_smoke.plan.output_width << L"x"
               << convert_smoke.plan.output_height << L" NV12\n";
  }
  const auto processor_smoke =
      olouie::graphics::RunGpuBgraToNv12VideoProcessorSmoke(
          d3d.device(), d3d.immediate_context(),
          {static_cast<uint32_t>(monitor->Width()),
           static_cast<uint32_t>(monitor->Height()),
           static_cast<uint32_t>(monitor->Width() & ~1),
           static_cast<uint32_t>(monitor->Height() & ~1)});
  std::wcout << L"BGRA-to-NV12 VideoProcessor smoke: "
             << olouie::graphics::GpuBgraToNv12StatusName(
                    processor_smoke.status)
             << L'\n';
  if (!processor_smoke.message.empty()) {
    std::wcout << L"  " << processor_smoke.message << L'\n';
  }
  if (processor_smoke.Succeeded()) {
    std::wcout << L"  conversion executed: "
               << (processor_smoke.conversion_executed ? L"yes" : L"no")
               << L"\n  input view reused: "
               << (processor_smoke.input_view_reused ? L"yes" : L"no")
               << L"\n  output view reused: "
               << (processor_smoke.output_view_reused ? L"yes" : L"no")
               << L'\n';
  }
  std::wcout << L"WGC supported: "
             << (olouie::capture::IsWgcSupported() ? L"yes" : L"no") << L'\n';

  if (!run_wgc) {
    std::wcout << L"Run with --wgc 3000 to start a manual capture smoke.\n";
    return 0;
  }

  std::wcout << L"Starting WGC smoke for " << duration.count() << L" ms...\n";
  const auto result = olouie::capture::RunWgcMonitorSmoke(
      monitor->handle, d3d.device(), duration);

  if (!result.error.empty()) {
    std::wcerr << result.error << L'\n';
    return 1;
  }

  std::wcout << L"WGC frames: " << result.frame_count << L'\n'
             << L"First timestamp ticks: " << result.first_timestamp_ticks
             << L'\n'
             << L"Last timestamp ticks: " << result.last_timestamp_ticks
             << L'\n';

  return result.frame_count > 0 ? 0 : 1;
}
