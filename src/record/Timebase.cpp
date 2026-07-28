#include "record/Timebase.h"

#include <cmath>
#include <string>

namespace olouie::record {
namespace {

constexpr long double kNanosecondsPerSecond = 1000000000.0L;

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

int64_t ScaleToNs(int64_t value, int64_t frequency) noexcept {
  if (frequency <= 0) {
    return 0;
  }

  const long double scaled =
      (static_cast<long double>(value) * kNanosecondsPerSecond) /
      static_cast<long double>(frequency);
  return static_cast<int64_t>(std::llround(scaled));
}

}  // namespace

Timebase::Timebase(int64_t qpc_frequency, int64_t session_start_qpc)
    : qpc_frequency_(qpc_frequency), session_start_qpc_(session_start_qpc) {}

Timebase Timebase::FromQpc(int64_t qpc_frequency, int64_t session_start_qpc,
                           std::wstring* error) {
  if (qpc_frequency <= 0) {
    SetError(error, L"QPC frequency must be greater than zero.");
    return {};
  }

  return Timebase(qpc_frequency, session_start_qpc);
}

bool Timebase::IsValid() const noexcept {
  return qpc_frequency_ > 0;
}

int64_t Timebase::QpcToNs(int64_t qpc_value) const noexcept {
  return QpcDurationToNs(qpc_value - session_start_qpc_);
}

int64_t Timebase::QpcDurationToNs(int64_t qpc_ticks) const noexcept {
  return ScaleToNs(qpc_ticks, qpc_frequency_);
}

int64_t Timebase::SamplesToNs(int64_t sample_count,
                              int32_t sample_rate) const noexcept {
  return ScaleToNs(sample_count, sample_rate);
}

int64_t Timebase::qpc_frequency() const noexcept {
  return qpc_frequency_;
}

int64_t Timebase::session_start_qpc() const noexcept {
  return session_start_qpc_;
}

}  // namespace olouie::record
