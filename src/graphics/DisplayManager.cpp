#include "graphics/DisplayManager.h"

#include <algorithm>
#include <string>

namespace olouie::graphics {
namespace {

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT,
                              LPARAM user_data) {
  auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(user_data);

  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return TRUE;
  }

  MonitorInfo monitor_info;
  monitor_info.handle = monitor;
  monitor_info.monitor_rect = info.rcMonitor;
  monitor_info.work_rect = info.rcWork;
  monitor_info.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  monitor_info.device_name = info.szDevice;
  monitors->push_back(std::move(monitor_info));
  return TRUE;
}

}  // namespace

int MonitorInfo::Width() const noexcept {
  return monitor_rect.right - monitor_rect.left;
}

int MonitorInfo::Height() const noexcept {
  return monitor_rect.bottom - monitor_rect.top;
}

std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> monitors;
  EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc,
                      reinterpret_cast<LPARAM>(&monitors));
  return monitors;
}

const MonitorInfo* FindPrimaryMonitor(
    const std::vector<MonitorInfo>& monitors) {
  const auto found =
      std::find_if(monitors.begin(), monitors.end(),
                   [](const MonitorInfo& monitor) { return monitor.primary; });

  if (found != monitors.end()) {
    return &(*found);
  }

  if (!monitors.empty()) {
    return &monitors.front();
  }

  return nullptr;
}

winrt::com_ptr<IDXGIAdapter1> FindDxgiAdapterForMonitor(HMONITOR monitor) {
  winrt::com_ptr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
    return nullptr;
  }

  for (UINT adapter_index = 0;; ++adapter_index) {
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(adapter_index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
      break;
    }

    for (UINT output_index = 0;; ++output_index) {
      winrt::com_ptr<IDXGIOutput> output;
      if (adapter->EnumOutputs(output_index, output.put()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }

      DXGI_OUTPUT_DESC output_desc{};
      if (SUCCEEDED(output->GetDesc(&output_desc)) &&
          output_desc.Monitor == monitor) {
        return adapter;
      }
    }
  }

  return nullptr;
}

std::wstring AdapterDescription(IDXGIAdapter1* adapter) {
  if (adapter == nullptr) {
    return L"default hardware adapter";
  }

  DXGI_ADAPTER_DESC1 desc{};
  if (FAILED(adapter->GetDesc1(&desc))) {
    return L"unknown adapter";
  }

  return desc.Description;
}

}  // namespace olouie::graphics
