#pragma once

#include <cstdint>
#include <string>

namespace olouie::record {

constexpr int64_t kSystemRelativeTimestampFrequency = 10000000;

struct SessionClock {
  int64_t qpc_frequency = 0;
  int64_t origin_qpc = 0;
  int64_t origin_100ns = 0;
  int64_t origin_ns = 0;

  bool IsValid() const noexcept;
};

bool CaptureSessionClock(SessionClock* clock, std::wstring* error);

bool BuildSessionClock(int64_t qpc_frequency,
                       int64_t origin_qpc,
                       SessionClock* clock,
                       std::wstring* error);

}  // namespace olouie::record
