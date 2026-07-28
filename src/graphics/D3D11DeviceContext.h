#pragma once

#include <windows.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <string>

namespace olouie::graphics {

class D3D11DeviceContext final {
 public:
  D3D11DeviceContext() = default;
  D3D11DeviceContext(D3D11DeviceContext&&) noexcept = default;
  D3D11DeviceContext& operator=(D3D11DeviceContext&&) noexcept = default;

  D3D11DeviceContext(const D3D11DeviceContext&) = delete;
  D3D11DeviceContext& operator=(const D3D11DeviceContext&) = delete;

  static D3D11DeviceContext CreateForMonitor(HMONITOR monitor,
                                             std::wstring* error);

  bool IsValid() const noexcept;
  ID3D11Device* device() const noexcept;
  ID3D11DeviceContext* immediate_context() const noexcept;
  D3D_FEATURE_LEVEL feature_level() const noexcept;
  const std::wstring& adapter_description() const noexcept;

 private:
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> immediate_context_;
  D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_9_1;
  std::wstring adapter_description_;
};

}  // namespace olouie::graphics
