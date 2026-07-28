#pragma once

#include <chrono>
#include <string>

#include "audio/CapturedPcmSink.h"
#include "audio/PcmAudio.h"

namespace olouie::audio {

using LoopbackSmokeResult = PcmCaptureStats;

bool TryGetDefaultRenderLoopbackFormat(PcmStreamFormat* format,
                                       std::wstring* error);

bool RunDefaultRenderLoopbackSmoke(std::chrono::milliseconds duration,
                                   LoopbackSmokeResult* result,
                                   std::wstring* error);

bool RunDefaultRenderLoopbackSmoke(std::chrono::milliseconds duration,
                                   LoopbackSmokeResult* result,
                                   ICapturedPcmSink* sink,
                                   std::wstring* error);

}  // namespace olouie::audio
