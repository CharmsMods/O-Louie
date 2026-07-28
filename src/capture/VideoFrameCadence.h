#pragma once

#include <cstdint>

namespace olouie::capture {

struct VideoFrameCadenceConfig {
  uint64_t timestamp_frequency = 0;
  uint32_t target_fps_numerator = 0;
  uint32_t target_fps_denominator = 0;

  bool IsValid() const noexcept;
};

enum class VideoFrameCadenceDecision {
  Accepted,
  RateLimited,
  InvalidTimestamp,
  InvalidConfig,
};

struct VideoFrameCadenceStats {
  uint64_t evaluated_frame_count = 0;
  uint64_t accepted_frame_count = 0;
  uint64_t rate_limited_frame_count = 0;
  uint64_t invalid_timestamp_count = 0;
  uint64_t invalid_config_count = 0;
  uint64_t delayed_rebase_count = 0;
};

class VideoFrameCadence final {
 public:
  VideoFrameCadence() = default;
  explicit VideoFrameCadence(VideoFrameCadenceConfig config);

  bool Configure(VideoFrameCadenceConfig config) noexcept;
  void Reset() noexcept;

  VideoFrameCadenceDecision Evaluate(int64_t timestamp_ticks) noexcept;

  bool IsConfigured() const noexcept;
  const VideoFrameCadenceConfig& config() const noexcept;
  const VideoFrameCadenceStats& stats() const noexcept;

 private:
  void AcceptFirst(int64_t timestamp_ticks) noexcept;
  void RebaseAt(int64_t timestamp_ticks) noexcept;

  VideoFrameCadenceConfig config_;
  VideoFrameCadenceStats stats_;
  uint64_t interval_scaled_ = 0;
  uint64_t next_due_scaled_ = 0;
  int64_t base_timestamp_ticks_ = 0;
  int64_t last_timestamp_ticks_ = 0;
  bool started_ = false;
};

const wchar_t* VideoFrameCadenceDecisionName(
    VideoFrameCadenceDecision decision) noexcept;

}  // namespace olouie::capture
