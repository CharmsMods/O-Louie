#include "app/SingleInstance.h"

#include <string>

namespace olouie::app {

SingleInstance::~SingleInstance() {
  if (mutex_ != nullptr) {
    if (acquired_) {
      ReleaseMutex(mutex_);
    }
    CloseHandle(mutex_);
  }
}

bool SingleInstance::Acquire(std::wstring_view mutex_name) {
  if (mutex_ != nullptr) {
    return acquired_;
  }

  const std::wstring name(mutex_name);
  mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
  if (mutex_ == nullptr) {
    return false;
  }

  const DWORD last_error = GetLastError();
  if (last_error == ERROR_ALREADY_EXISTS || last_error == ERROR_ACCESS_DENIED) {
    CloseHandle(mutex_);
    mutex_ = nullptr;
    return false;
  }

  acquired_ = true;
  return true;
}

bool SingleInstance::IsAcquired() const noexcept {
  return acquired_;
}

}  // namespace olouie::app
