#include "graphics/D3D11DeviceContext.h"

#include <d3d11_4.h>
#include <dxgi.h>

#include <string>

#include "graphics/DisplayManager.h"

namespace olouie::graphics {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

HRESULT CreateDevice(IDXGIAdapter1* adapter, UINT flags,
                     winrt::com_ptr<ID3D11Device>& device,
                     winrt::com_ptr<ID3D11DeviceContext>& context,
                     D3D_FEATURE_LEVEL* feature_level) {
  const D3D_FEATURE_LEVEL requested_levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  const D3D_DRIVER_TYPE driver_type =
      adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;

  return D3D11CreateDevice(adapter, driver_type, nullptr, flags,
                           requested_levels,
                           static_cast<UINT>(std::size(requested_levels)),
                           D3D11_SDK_VERSION, device.put(), feature_level,
                           context.put());
}

std::wstring DescribeHRESULT(HRESULT result) {
  return L"0x" + std::to_wstring(static_cast<unsigned long>(result));
}

}  // namespace

D3D11DeviceContext D3D11DeviceContext::CreateForMonitor(HMONITOR monitor,
                                                        std::wstring* error) {
  D3D11DeviceContext created;
  auto adapter = FindDxgiAdapterForMonitor(monitor);

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
#ifdef D3D11_CREATE_DEVICE_VIDEO_SUPPORT
  flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#endif

  HRESULT result = CreateDevice(adapter.get(), flags, created.device_,
                                created.immediate_context_,
                                &created.feature_level_);

#ifdef _DEBUG
  if (FAILED(result)) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    created.device_ = nullptr;
    created.immediate_context_ = nullptr;
    result = CreateDevice(adapter.get(), flags, created.device_,
                          created.immediate_context_, &created.feature_level_);
  }
#endif

  if (FAILED(result)) {
    SetError(error, L"D3D11CreateDevice failed with HRESULT " +
                        DescribeHRESULT(result) + L".");
    return {};
  }

  winrt::com_ptr<ID3D11Multithread> multithread;
  if (SUCCEEDED(created.device_->QueryInterface(
          __uuidof(ID3D11Multithread), multithread.put_void()))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  created.adapter_description_ = AdapterDescription(adapter.get());
  return created;
}

bool D3D11DeviceContext::IsValid() const noexcept {
  return device_ != nullptr && immediate_context_ != nullptr;
}

ID3D11Device* D3D11DeviceContext::device() const noexcept {
  return device_.get();
}

ID3D11DeviceContext* D3D11DeviceContext::immediate_context() const noexcept {
  return immediate_context_.get();
}

D3D_FEATURE_LEVEL D3D11DeviceContext::feature_level() const noexcept {
  return feature_level_;
}

const std::wstring& D3D11DeviceContext::adapter_description() const noexcept {
  return adapter_description_;
}

}  // namespace olouie::graphics
