#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/AacEncodeSink.h"
#include "audio/PreparedPcmQueue.h"

namespace olouie::audio {

enum class AudioEncodeWorkerStatus {
  Success,
  InvalidArgument,
  SinkError,
};

struct AudioEncodeWorkerResult {
  AudioEncodeWorkerStatus status = AudioEncodeWorkerStatus::InvalidArgument;
  std::wstring message;
  size_t submitted_block_count = 0;

  bool Succeeded() const noexcept;
};

struct AudioEncodeWorkerStats {
  uint64_t submitted_block_count = 0;
  uint64_t submitted_frame_count = 0;
  uint64_t sink_error_count = 0;
};

class AudioEncodeWorker final {
 public:
  AudioEncodeWorker(PreparedPcmQueue* queue, AacEncodeSink* sink);

  AudioEncodeWorkerResult DrainQueuedBlocks(size_t max_blocks);
  AudioEncodeWorkerResult DrainAllQueuedBlocks();

  const AudioEncodeWorkerStats& stats() const noexcept;

 private:
  PreparedPcmQueue* queue_ = nullptr;
  AacEncodeSink* sink_ = nullptr;
  AudioEncodeWorkerStats stats_;
};

}  // namespace olouie::audio
