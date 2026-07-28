#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "audio/AacEncoder.h"

namespace olouie::audio {

enum class PreparedPcmOverflowPolicy {
  RejectNewest,
  DropOldest,
};

enum class PreparedPcmQueueStatus {
  Success,
  InvalidConfig,
  InvalidBlock,
  QueueFull,
};

struct PreparedPcmBlock {
  AacPcmInput input;
  std::vector<std::byte> data;

  bool IsValid() const noexcept;
};

struct PreparedPcmQueueResult {
  PreparedPcmQueueStatus status = PreparedPcmQueueStatus::InvalidConfig;
  std::wstring message;

  bool Succeeded() const noexcept;
};

struct PreparedPcmQueueStats {
  uint64_t pushed_block_count = 0;
  uint64_t popped_block_count = 0;
  uint64_t rejected_block_count = 0;
  uint64_t dropped_block_count = 0;
};

class PreparedPcmQueue final {
 public:
  PreparedPcmQueue(size_t capacity,
                   PreparedPcmOverflowPolicy overflow_policy =
                       PreparedPcmOverflowPolicy::RejectNewest);

  PreparedPcmQueue(const PreparedPcmQueue&) = delete;
  PreparedPcmQueue& operator=(const PreparedPcmQueue&) = delete;

  PreparedPcmQueueResult TryPush(const PreparedPcmBlock& block);
  PreparedPcmQueueResult TryPush(AacPcmInput input,
                                 std::span<const std::byte> data);
  bool TryPop(PreparedPcmBlock* block);
  void Clear();

  size_t size() const;
  size_t capacity() const noexcept;
  bool empty() const;
  bool full() const;
  PreparedPcmQueueStats SnapshotStats() const;

 private:
  PreparedPcmQueueResult PushBlockLocked(PreparedPcmBlock block);

  size_t capacity_ = 0;
  PreparedPcmOverflowPolicy overflow_policy_ =
      PreparedPcmOverflowPolicy::RejectNewest;
  mutable std::mutex mutex_;
  std::deque<PreparedPcmBlock> blocks_;
  PreparedPcmQueueStats stats_;
};

}  // namespace olouie::audio
