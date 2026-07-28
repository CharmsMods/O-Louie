#include "audio/PreparedPcmQueue.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

PreparedPcmQueueResult Result(PreparedPcmQueueStatus status,
                              std::wstring message) {
  PreparedPcmQueueResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

}  // namespace

bool PreparedPcmBlock::IsValid() const noexcept {
  return input.frame_count > 0 && input.duration_ns > 0 && !data.empty();
}

bool PreparedPcmQueueResult::Succeeded() const noexcept {
  return status == PreparedPcmQueueStatus::Success;
}

PreparedPcmQueue::PreparedPcmQueue(
    size_t capacity, PreparedPcmOverflowPolicy overflow_policy)
    : capacity_(capacity), overflow_policy_(overflow_policy) {}

PreparedPcmQueueResult PreparedPcmQueue::TryPush(
    const PreparedPcmBlock& block) {
  std::lock_guard lock(mutex_);
  return PushBlockLocked(block);
}

PreparedPcmQueueResult PreparedPcmQueue::TryPush(
    AacPcmInput input, std::span<const std::byte> data) {
  PreparedPcmBlock block;
  block.input = input;
  block.data.assign(data.begin(), data.end());
  std::lock_guard lock(mutex_);
  return PushBlockLocked(std::move(block));
}

bool PreparedPcmQueue::TryPop(PreparedPcmBlock* block) {
  std::lock_guard lock(mutex_);
  if (block == nullptr || blocks_.empty()) {
    return false;
  }

  *block = std::move(blocks_.front());
  blocks_.pop_front();
  ++stats_.popped_block_count;
  return true;
}

void PreparedPcmQueue::Clear() {
  std::lock_guard lock(mutex_);
  blocks_.clear();
}

size_t PreparedPcmQueue::size() const {
  std::lock_guard lock(mutex_);
  return blocks_.size();
}

size_t PreparedPcmQueue::capacity() const noexcept {
  return capacity_;
}

bool PreparedPcmQueue::empty() const {
  std::lock_guard lock(mutex_);
  return blocks_.empty();
}

bool PreparedPcmQueue::full() const {
  std::lock_guard lock(mutex_);
  return capacity_ > 0 && blocks_.size() >= capacity_;
}

PreparedPcmQueueStats PreparedPcmQueue::SnapshotStats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

PreparedPcmQueueResult PreparedPcmQueue::PushBlockLocked(
    PreparedPcmBlock block) {
  if (capacity_ == 0) {
    ++stats_.rejected_block_count;
    return Result(PreparedPcmQueueStatus::InvalidConfig,
                  L"Prepared PCM queue capacity must be greater than zero.");
  }

  if (!block.IsValid()) {
    ++stats_.rejected_block_count;
    return Result(PreparedPcmQueueStatus::InvalidBlock,
                  L"Prepared PCM queue block is invalid.");
  }

  if (blocks_.size() >= capacity_) {
    if (overflow_policy_ == PreparedPcmOverflowPolicy::RejectNewest) {
      ++stats_.rejected_block_count;
      return Result(PreparedPcmQueueStatus::QueueFull,
                    L"Prepared PCM queue is full.");
    }

    blocks_.pop_front();
    ++stats_.dropped_block_count;
  }

  blocks_.push_back(std::move(block));
  ++stats_.pushed_block_count;
  return Result(PreparedPcmQueueStatus::Success, L"");
}

}  // namespace olouie::audio
