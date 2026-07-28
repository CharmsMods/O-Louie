#include "graphics/D3D11DeviceFault.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <utility>

namespace olouie::graphics {
namespace {

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

D3D11DeviceFaultKind FaultKindForResult(HRESULT result) noexcept {
  switch (result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return D3D11DeviceFaultKind::Removed;
    case DXGI_ERROR_DEVICE_RESET:
      return D3D11DeviceFaultKind::Reset;
    case DXGI_ERROR_DEVICE_HUNG:
      return D3D11DeviceFaultKind::Hung;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      return D3D11DeviceFaultKind::DriverInternalError;
    default:
      return D3D11DeviceFaultKind::None;
  }
}

const wchar_t* FaultDescription(D3D11DeviceFaultKind kind) noexcept {
  switch (kind) {
    case D3D11DeviceFaultKind::Removed:
      return L"was removed";
    case D3D11DeviceFaultKind::Reset:
      return L"was reset";
    case D3D11DeviceFaultKind::Hung:
      return L"hung";
    case D3D11DeviceFaultKind::DriverInternalError:
      return L"reported an internal driver error";
    case D3D11DeviceFaultKind::Unknown:
      return L"became unavailable";
    case D3D11DeviceFaultKind::None:
      break;
  }
  return L"is healthy";
}

}  // namespace

bool D3D11DeviceFault::Failed() const noexcept {
  return kind != D3D11DeviceFaultKind::None;
}

D3D11DeviceFault ClassifyD3D11DeviceFault(
    HRESULT operation_result,
    HRESULT removal_reason,
    std::wstring operation) {
  D3D11DeviceFault fault;
  fault.operation_result = operation_result;
  fault.removal_reason = removal_reason;
  fault.operation = std::move(operation);

  fault.kind = FaultKindForResult(operation_result);
  if (fault.kind == D3D11DeviceFaultKind::None) {
    fault.kind = FaultKindForResult(removal_reason);
  }
  if (fault.kind == D3D11DeviceFaultKind::None && FAILED(removal_reason)) {
    fault.kind = D3D11DeviceFaultKind::Unknown;
  }
  if (!fault.Failed()) {
    return fault;
  }

  fault.message = L"The D3D11 device " +
                  std::wstring(FaultDescription(fault.kind));
  if (!fault.operation.empty()) {
    fault.message += L" during " + fault.operation;
  }
  fault.message += L" (operation HRESULT " +
                   HResultToHex(operation_result) +
                   L", device removal reason " +
                   HResultToHex(removal_reason) + L").";
  return fault;
}

D3D11DeviceFault InspectD3D11DeviceFault(
    ID3D11Device* device,
    HRESULT operation_result,
    std::wstring operation) {
  const HRESULT removal_reason =
      device == nullptr ? S_OK : device->GetDeviceRemovedReason();
  return ClassifyD3D11DeviceFault(operation_result, removal_reason,
                                  std::move(operation));
}

bool IsD3D11DeviceLossResult(HRESULT result) noexcept {
  return FaultKindForResult(result) != D3D11DeviceFaultKind::None;
}

const wchar_t* D3D11DeviceFaultKindName(
    D3D11DeviceFaultKind kind) noexcept {
  switch (kind) {
    case D3D11DeviceFaultKind::None:
      return L"none";
    case D3D11DeviceFaultKind::Removed:
      return L"removed";
    case D3D11DeviceFaultKind::Reset:
      return L"reset";
    case D3D11DeviceFaultKind::Hung:
      return L"hung";
    case D3D11DeviceFaultKind::DriverInternalError:
      return L"driver internal error";
    case D3D11DeviceFaultKind::Unknown:
      return L"unknown device loss";
  }
  return L"unknown";
}

}  // namespace olouie::graphics
