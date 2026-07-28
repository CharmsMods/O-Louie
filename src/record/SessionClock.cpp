#include "record/SessionClock.h"

#include <windows.h>

#include <cmath>
#include <limits>
#include <utility>

namespace olouie::record {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

bool SessionClock::IsValid() const noexcept {
  return qpc_frequency > 0 && origin_qpc >= 0 && origin_100ns >= 0 &&
         origin_ns >= 0 && origin_ns / 100 == origin_100ns;
}

bool BuildSessionClock(int64_t qpc_frequency,
                       int64_t origin_qpc,
                       SessionClock* clock,
                       std::wstring* error) {
  if (clock == nullptr) {
    SetError(error, L"Session clock needs an output destination.");
    return false;
  }
  *clock = {};

  if (qpc_frequency <= 0 || origin_qpc < 0) {
    SetError(error, L"Session clock needs a positive QPC frequency and a "
                    L"nonnegative counter value.");
    return false;
  }

  const long double scaled =
      (static_cast<long double>(origin_qpc) *
       static_cast<long double>(kSystemRelativeTimestampFrequency)) /
      static_cast<long double>(qpc_frequency);
  if (scaled < 0.0L ||
      scaled > static_cast<long double>(
                   std::numeric_limits<int64_t>::max() / 100)) {
    SetError(error, L"Session clock counter value is out of range.");
    return false;
  }

  SessionClock built;
  built.qpc_frequency = qpc_frequency;
  built.origin_qpc = origin_qpc;
  built.origin_100ns = static_cast<int64_t>(std::llround(scaled));
  built.origin_ns = built.origin_100ns * 100;
  if (!built.IsValid()) {
    SetError(error, L"Session clock conversion produced invalid state.");
    return false;
  }

  *clock = built;
  return true;
}

bool CaptureSessionClock(SessionClock* clock, std::wstring* error) {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
    SetError(error, L"QueryPerformanceFrequency failed.");
    return false;
  }
  if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
    SetError(error, L"QueryPerformanceCounter failed.");
    return false;
  }

  return BuildSessionClock(frequency.QuadPart, counter.QuadPart, clock,
                           error);
}

}  // namespace olouie::record
