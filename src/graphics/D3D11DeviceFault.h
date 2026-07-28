#pragma once

#include <windows.h>

#include <string>

struct ID3D11Device;

namespace olouie::graphics {

enum class D3D11DeviceFaultKind {
  None,
  Removed,
  Reset,
  Hung,
  DriverInternalError,
  Unknown,
};

struct D3D11DeviceFault {
  D3D11DeviceFaultKind kind = D3D11DeviceFaultKind::None;
  HRESULT operation_result = S_OK;
  HRESULT removal_reason = S_OK;
  std::wstring operation;
  std::wstring message;

  bool Failed() const noexcept;
};

D3D11DeviceFault ClassifyD3D11DeviceFault(
    HRESULT operation_result,
    HRESULT removal_reason,
    std::wstring operation);
D3D11DeviceFault InspectD3D11DeviceFault(
    ID3D11Device* device,
    HRESULT operation_result,
    std::wstring operation);
bool IsD3D11DeviceLossResult(HRESULT result) noexcept;
const wchar_t* D3D11DeviceFaultKindName(
    D3D11DeviceFaultKind kind) noexcept;

}  // namespace olouie::graphics
