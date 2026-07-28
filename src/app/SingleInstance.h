#pragma once

#include <windows.h>

#include <string_view>

namespace olouie::app {

class SingleInstance final {
 public:
  SingleInstance() = default;
  ~SingleInstance();

  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  bool Acquire(std::wstring_view mutex_name);
  bool IsAcquired() const noexcept;

 private:
  HANDLE mutex_ = nullptr;
  bool acquired_ = false;
};

}  // namespace olouie::app
