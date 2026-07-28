#include "audio/CapturedPcmSessionSink.h"

#include <string>
#include <utility>

namespace olouie::audio {
namespace {

CapturedPcmSinkResult Result(CapturedPcmSinkStatus status,
                             std::wstring message) {
  CapturedPcmSinkResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

CapturedPcmSinkStatus SinkStatusFromDispatch(
    AudioSourceSessionDispatchStatus status) noexcept {
  switch (status) {
    case AudioSourceSessionDispatchStatus::Success:
      return CapturedPcmSinkStatus::Success;
    case AudioSourceSessionDispatchStatus::InvalidConfig:
      return CapturedPcmSinkStatus::InvalidConfig;
    case AudioSourceSessionDispatchStatus::RouteError:
      return CapturedPcmSinkStatus::RouteError;
    case AudioSourceSessionDispatchStatus::QueueError:
      return CapturedPcmSinkStatus::QueueError;
  }

  return CapturedPcmSinkStatus::SinkError;
}

}  // namespace

CapturedPcmSessionSink::CapturedPcmSessionSink(
    AudioSourceSessionDispatcher* dispatcher,
    int64_t qpc_origin_ns)
    : dispatcher_(dispatcher), qpc_origin_ns_(qpc_origin_ns) {}

bool CapturedPcmSessionSink::IsConfigured() const noexcept {
  return dispatcher_ != nullptr && dispatcher_->IsConfigured();
}

CapturedPcmSinkResult CapturedPcmSessionSink::OnCapturedPcm(
    const CapturedPcmPacket& packet) {
  ++stats_.received_packet_count;

  if (!packet.IsValid()) {
    ++stats_.invalid_packet_count;
    return Result(CapturedPcmSinkStatus::InvalidPacket,
                  L"Captured PCM packet is invalid.");
  }

  if (dispatcher_ == nullptr) {
    ++stats_.invalid_config_count;
    return Result(CapturedPcmSinkStatus::InvalidConfig,
                  L"Captured PCM session sink needs a dispatcher.");
  }

  const int64_t pts_ns = packet.packet.timing.qpc_position_ns - qpc_origin_ns_;
  last_dispatch_ = dispatcher_->QueueCapturedPcm(
      packet.source, packet.format, packet.packet, pts_ns, packet.pcm_bytes);

  switch (last_dispatch_.status) {
    case AudioSourceSessionDispatchStatus::Success:
      ++stats_.queued_packet_count;
      return Result(CapturedPcmSinkStatus::Success, L"");
    case AudioSourceSessionDispatchStatus::InvalidConfig:
      ++stats_.invalid_config_count;
      break;
    case AudioSourceSessionDispatchStatus::RouteError:
      ++stats_.route_error_count;
      break;
    case AudioSourceSessionDispatchStatus::QueueError:
      ++stats_.queue_error_count;
      break;
  }

  return Result(SinkStatusFromDispatch(last_dispatch_.status),
                last_dispatch_.message);
}

const CapturedPcmSessionSinkStats& CapturedPcmSessionSink::stats()
    const noexcept {
  return stats_;
}

const AudioSourceSessionDispatchResult&
CapturedPcmSessionSink::last_dispatch_result() const noexcept {
  return last_dispatch_;
}

}  // namespace olouie::audio
