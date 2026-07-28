#include "audio/AudioEncodeWorker.h"

#include <limits>
#include <string>
#include <utility>

namespace olouie::audio {
namespace {

AudioEncodeWorkerResult Result(AudioEncodeWorkerStatus status,
                               std::wstring message,
                               size_t submitted_block_count) {
  AudioEncodeWorkerResult result;
  result.status = status;
  result.message = std::move(message);
  result.submitted_block_count = submitted_block_count;
  return result;
}

}  // namespace

bool AudioEncodeWorkerResult::Succeeded() const noexcept {
  return status == AudioEncodeWorkerStatus::Success;
}

AudioEncodeWorker::AudioEncodeWorker(PreparedPcmQueue* queue,
                                     AacEncodeSink* sink)
    : queue_(queue), sink_(sink) {}

AudioEncodeWorkerResult AudioEncodeWorker::DrainQueuedBlocks(
    size_t max_blocks) {
  if (queue_ == nullptr || sink_ == nullptr) {
    return Result(AudioEncodeWorkerStatus::InvalidArgument,
                  L"Audio encode worker needs a PCM queue and AAC sink.", 0);
  }

  size_t submitted_blocks = 0;
  while (submitted_blocks < max_blocks) {
    PreparedPcmBlock block;
    if (!queue_->TryPop(&block)) {
      break;
    }

    const auto submit = sink_->SubmitPreparedPcm(block.input, block.data);
    if (!submit.Succeeded()) {
      ++stats_.sink_error_count;
      return Result(AudioEncodeWorkerStatus::SinkError, submit.message,
                    submitted_blocks);
    }

    ++submitted_blocks;
    ++stats_.submitted_block_count;
    stats_.submitted_frame_count += block.input.frame_count;
  }

  return Result(AudioEncodeWorkerStatus::Success, L"", submitted_blocks);
}

AudioEncodeWorkerResult AudioEncodeWorker::DrainAllQueuedBlocks() {
  return DrainQueuedBlocks(std::numeric_limits<size_t>::max());
}

const AudioEncodeWorkerStats& AudioEncodeWorker::stats() const noexcept {
  return stats_;
}

}  // namespace olouie::audio
