#include "capture/VideoFrameCadence.h"

#include <limits>

namespace olouie::capture {

bool VideoFrameCadenceConfig::IsValid() const noexcept {
  if (timestamp_frequency == 0 || target_fps_numerator == 0 ||
      target_fps_denominator == 0) {
    return false;
  }
  if (timestamp_frequency >
      std::numeric_limits<uint64_t>::max() /
          target_fps_denominator) {
    return false;
  }

  const uint64_t interval_scaled =
      timestamp_frequency * target_fps_denominator;
  return interval_scaled >= target_fps_numerator;
}

VideoFrameCadence::VideoFrameCadence(VideoFrameCadenceConfig config) {
  (void)Configure(config);
}

bool VideoFrameCadence::Configure(VideoFrameCadenceConfig config) noexcept {
  config_ = config;
  stats_ = {};
  interval_scaled_ = 0;
  next_due_scaled_ = 0;
  base_timestamp_ticks_ = 0;
  last_timestamp_ticks_ = 0;
  started_ = false;

  if (!config_.IsValid()) {
    return false;
  }
  interval_scaled_ =
      config_.timestamp_frequency * config_.target_fps_denominator;
  return true;
}

void VideoFrameCadence::Reset() noexcept {
  const auto config = config_;
  (void)Configure(config);
}

VideoFrameCadenceDecision VideoFrameCadence::Evaluate(
    int64_t timestamp_ticks) noexcept {
  ++stats_.evaluated_frame_count;
  if (!IsConfigured()) {
    ++stats_.invalid_config_count;
    return VideoFrameCadenceDecision::InvalidConfig;
  }
  if (timestamp_ticks < 0 ||
      (started_ && timestamp_ticks < last_timestamp_ticks_)) {
    ++stats_.invalid_timestamp_count;
    return VideoFrameCadenceDecision::InvalidTimestamp;
  }

  last_timestamp_ticks_ = timestamp_ticks;
  if (!started_) {
    AcceptFirst(timestamp_ticks);
    return VideoFrameCadenceDecision::Accepted;
  }

  const uint64_t elapsed_ticks =
      static_cast<uint64_t>(timestamp_ticks) -
      static_cast<uint64_t>(base_timestamp_ticks_);
  if (elapsed_ticks > std::numeric_limits<uint64_t>::max() /
                          config_.target_fps_numerator) {
    ++stats_.accepted_frame_count;
    ++stats_.delayed_rebase_count;
    RebaseAt(timestamp_ticks);
    return VideoFrameCadenceDecision::Accepted;
  }

  const uint64_t elapsed_scaled =
      elapsed_ticks * config_.target_fps_numerator;
  const uint64_t rounding_tolerance = config_.target_fps_numerator;
  if (elapsed_scaled < next_due_scaled_ &&
      next_due_scaled_ - elapsed_scaled > rounding_tolerance) {
    ++stats_.rate_limited_frame_count;
    return VideoFrameCadenceDecision::RateLimited;
  }

  ++stats_.accepted_frame_count;
  const uint64_t lateness =
      elapsed_scaled > next_due_scaled_
          ? elapsed_scaled - next_due_scaled_
          : 0;
  if (lateness >= interval_scaled_ ||
      next_due_scaled_ >
          std::numeric_limits<uint64_t>::max() - interval_scaled_) {
    ++stats_.delayed_rebase_count;
    RebaseAt(timestamp_ticks);
  } else {
    next_due_scaled_ += interval_scaled_;
  }
  return VideoFrameCadenceDecision::Accepted;
}

bool VideoFrameCadence::IsConfigured() const noexcept {
  return interval_scaled_ != 0 && config_.IsValid();
}

const VideoFrameCadenceConfig& VideoFrameCadence::config() const noexcept {
  return config_;
}

const VideoFrameCadenceStats& VideoFrameCadence::stats() const noexcept {
  return stats_;
}

void VideoFrameCadence::AcceptFirst(int64_t timestamp_ticks) noexcept {
  started_ = true;
  base_timestamp_ticks_ = timestamp_ticks;
  last_timestamp_ticks_ = timestamp_ticks;
  next_due_scaled_ = interval_scaled_;
  ++stats_.accepted_frame_count;
}

void VideoFrameCadence::RebaseAt(int64_t timestamp_ticks) noexcept {
  base_timestamp_ticks_ = timestamp_ticks;
  last_timestamp_ticks_ = timestamp_ticks;
  next_due_scaled_ = interval_scaled_;
  started_ = true;
}

const wchar_t* VideoFrameCadenceDecisionName(
    VideoFrameCadenceDecision decision) noexcept {
  switch (decision) {
    case VideoFrameCadenceDecision::Accepted:
      return L"accepted";
    case VideoFrameCadenceDecision::RateLimited:
      return L"rate limited";
    case VideoFrameCadenceDecision::InvalidTimestamp:
      return L"invalid timestamp";
    case VideoFrameCadenceDecision::InvalidConfig:
      return L"invalid config";
  }
  return L"unknown";
}

}  // namespace olouie::capture
