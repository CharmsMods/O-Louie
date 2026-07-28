#pragma once

#include <chrono>
#include <string>

#include "audio/CapturedPcmSink.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

using MicCaptureSmokeResult = PcmCaptureStats;

bool TryGetDefaultMicCaptureFormat(PcmStreamFormat* format,
                                   std::wstring* error);

bool RunDefaultMicCaptureSmoke(std::chrono::milliseconds duration,
                               MicCaptureSmokeResult* result,
                               std::wstring* error);

bool RunDefaultMicCaptureSmoke(std::chrono::milliseconds duration,
                               MicCaptureSmokeResult* result,
                               ICapturedPcmSink* sink,
                               std::wstring* error);

}  // namespace olouie::audio
