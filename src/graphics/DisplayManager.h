#pragma once

#include <windows.h>

#include <dxgi1_6.h>
#include <winrt/base.h>

#include <string>
#include <vector>

namespace olouie::graphics {

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT monitor_rect{};
  RECT work_rect{};
  bool primary = false;
  std::wstring device_name;

  int Width() const noexcept;
  int Height() const noexcept;
};

std::vector<MonitorInfo> EnumerateMonitors();
const MonitorInfo* FindPrimaryMonitor(const std::vector<MonitorInfo>& monitors);
winrt::com_ptr<IDXGIAdapter1> FindDxgiAdapterForMonitor(HMONITOR monitor);
std::wstring AdapterDescription(IDXGIAdapter1* adapter);

}  // namespace olouie::graphics
