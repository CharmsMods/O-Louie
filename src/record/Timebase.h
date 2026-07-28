#pragma once

#include <cstdint>
#include <string>

namespace olouie::record {

class Timebase final {
 public:
  Timebase() = default;

  static Timebase FromQpc(int64_t qpc_frequency, int64_t session_start_qpc,
                          std::wstring* error);

  bool IsValid() const noexcept;
  int64_t QpcToNs(int64_t qpc_value) const noexcept;
  int64_t QpcDurationToNs(int64_t qpc_ticks) const noexcept;
  int64_t SamplesToNs(int64_t sample_count, int32_t sample_rate) const noexcept;

  int64_t qpc_frequency() const noexcept;
  int64_t session_start_qpc() const noexcept;

 private:
  Timebase(int64_t qpc_frequency, int64_t session_start_qpc);

  int64_t qpc_frequency_ = 0;
  int64_t session_start_qpc_ = 0;
};

}  // namespace olouie::record
