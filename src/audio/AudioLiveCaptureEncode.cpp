#include "audio/AudioLiveCaptureEncode.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "record/SessionClock.h"

namespace olouie::audio {
namespace {

constexpr std::chrono::milliseconds kMaxLiveCaptureEncodeDuration{30000};
constexpr std::chrono::milliseconds kMaxDrainInterval{1000};
constexpr int64_t kContinuityLagNs = 200000000;
constexpr int64_t kContinuityToleranceNs = 2000000;
constexpr int64_t kMaxSilenceBlockDurationNs = 100000000;
constexpr size_t kMaxSilenceBlocksPerTick = 8;
constexpr size_t kMaxSilenceBlocksPerCapturedPacket = 8;
constexpr size_t kMaxFinalSilenceBlocks = 128;

AudioLiveCaptureEncodeRunResult Result(
    AudioLiveCaptureEncodeStatus status,
    std::wstring message) {
  AudioLiveCaptureEncodeRunResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

WasapiCaptureSourceResult SourceResult(WasapiCaptureSourceStatus status,
                                       std::wstring message) {
  WasapiCaptureSourceResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

AudioLiveCaptureEncodeStatus StatusFromManager(
    AudioCaptureManagerStatus status) noexcept {
  switch (status) {
    case AudioCaptureManagerStatus::Success:
      return AudioLiveCaptureEncodeStatus::Success;
    case AudioCaptureManagerStatus::InvalidPlan:
    case AudioCaptureManagerStatus::MissingSink:
    case AudioCaptureManagerStatus::UnsupportedPlan:
    case AudioCaptureManagerStatus::DuplicateSource:
      return AudioLiveCaptureEncodeStatus::InvalidConfig;
  }
  return AudioLiveCaptureEncodeStatus::InvalidConfig;
}

bool SessionOptionsAreValid(
    const AudioLiveCaptureEncodeOptions& options) noexcept {
  return options.drain_interval > std::chrono::milliseconds(0) &&
         options.drain_interval <= kMaxDrainInterval &&
         options.max_blocks_per_drain_tick > 0 &&
         options.qpc_origin_ns >= 0 &&
         performance::IsValidCapturePerformanceMode(
             options.performance_mode);
}

bool RunOptionsAreValid(
    const AudioLiveCaptureEncodeOptions& options) noexcept {
  return SessionOptionsAreValid(options) &&
         options.duration > std::chrono::milliseconds(0) &&
         options.duration <= kMaxLiveCaptureEncodeDuration;
}

AudioLiveCaptureEncodeSourceResult SourceResultFromBinding(
    const AudioCaptureSourceBinding& binding) {
  AudioLiveCaptureEncodeSourceResult result;
  result.source = binding.source;
  result.runtime = binding.runtime;
  result.support = binding.support;
  result.track_id = binding.track_id;
  return result;
}

std::wstring SourceLabel(const AudioCaptureSourceBinding& binding) {
  return std::wstring(AudioCaptureSourceRuntimeName(binding.runtime)) +
         L" track " + std::to_wstring(binding.track_id);
}

bool CurrentSessionPtsNs(int64_t qpc_origin_ns,
                         int64_t* pts_ns,
                         std::wstring* error) {
  record::SessionClock now;
  if (!record::CaptureSessionClock(&now, error)) {
    return false;
  }
  if (now.origin_ns < qpc_origin_ns) {
    if (error != nullptr) {
      *error = L"Current QPC time precedes the audio session origin.";
    }
    return false;
  }
  *pts_ns = now.origin_ns - qpc_origin_ns;
  return true;
}

class WasapiLiveCaptureSource final : public IAudioLiveCaptureSource {
 public:
  explicit WasapiLiveCaptureSource(
      CapturedAudioSource source,
      performance::CapturePerformanceMode performance_mode =
          performance::CapturePerformanceMode::Balanced)
      : source_(source, performance_mode) {}

  CapturedAudioSource source() const noexcept override {
    return source_.source();
  }

  WasapiCaptureSourceResult Start(ICapturedPcmSink* sink) override {
    return source_.Start(sink);
  }

  void Stop() override {
    source_.Stop();
  }

  bool IsRunning() const noexcept override {
    return source_.IsRunning();
  }

  PcmCaptureStats SnapshotStats() const override {
    return source_.SnapshotStats();
  }

  WasapiCaptureSourceResult LastResult() const override {
    return source_.LastResult();
  }

 private:
  WasapiCaptureSource source_;
};

class SerializedBridgeSink final : public ICapturedPcmSink {
 public:
  SerializedBridgeSink(AudioCaptureEncodeBridge* bridge,
                       std::mutex* mutex,
                       int64_t qpc_origin_ns,
                       bool maintain_track_continuity)
      : bridge_(bridge),
        mutex_(mutex),
        qpc_origin_ns_(qpc_origin_ns),
        maintain_track_continuity_(maintain_track_continuity) {}

  bool AddSource(CapturedAudioSource source,
                 const PcmStreamFormat& format,
                 size_t result_index) {
    if (!format.IsValid()) {
      return false;
    }
    ContinuitySource state;
    state.source = source;
    state.format = format;
    state.result_index = result_index;
    continuity_sources_.push_back(state);
    return true;
  }

  CapturedPcmSinkResult OnCapturedPcm(
      const CapturedPcmPacket& packet) override {
    std::lock_guard lock(*mutex_);
    if (!maintain_track_continuity_) {
      return bridge_->captured_pcm_sink()->OnCapturedPcm(packet);
    }

    auto* state = FindSource(packet.source);
    if (state == nullptr) {
      return SinkFailure(CapturedPcmSinkStatus::RouteError,
                         L"Captured PCM source has no continuity state.");
    }
    if (packet.format.IsValid()) {
      state->format = packet.format;
    }

    CapturedPcmPacket adjusted = packet;
    int64_t packet_pts_ns =
        adjusted.packet.timing.qpc_position_ns - qpc_origin_ns_;
    if (adjusted.packet.timestamp_error ||
        packet_pts_ns + kContinuityToleranceNs < state->next_pts_ns) {
      packet_pts_ns = state->next_pts_ns;
      adjusted.packet.timing.qpc_position_ns =
          qpc_origin_ns_ + packet_pts_ns;
      adjusted.packet.timing.qpc_position_100ns = static_cast<uint64_t>(
          adjusted.packet.timing.qpc_position_ns / 100);
      ++state->retimed_packet_count;
    }

    const auto filled = FillSourceTo(
        state, packet_pts_ns, kMaxSilenceBlocksPerCapturedPacket);
    if (!filled.Succeeded()) {
      return filled;
    }
    if (state->next_pts_ns + kContinuityToleranceNs < packet_pts_ns) {
      packet_pts_ns = state->next_pts_ns;
      adjusted.packet.timing.qpc_position_ns =
          qpc_origin_ns_ + packet_pts_ns;
      adjusted.packet.timing.qpc_position_100ns = static_cast<uint64_t>(
          adjusted.packet.timing.qpc_position_ns / 100);
      ++state->retimed_packet_count;
    }

    const auto delivered =
        bridge_->captured_pcm_sink()->OnCapturedPcm(adjusted);
    if (delivered.Succeeded()) {
      state->next_pts_ns = std::max(
          state->next_pts_ns,
          packet_pts_ns + adjusted.packet.timing.duration_ns);
    }
    return delivered;
  }

  CapturedPcmSinkResult FillContinuityTo(int64_t target_pts_ns,
                                        size_t max_blocks_per_source,
                                        bool* complete) {
    std::lock_guard lock(*mutex_);
    bool all_complete = true;
    for (auto& source : continuity_sources_) {
      const auto filled =
          FillSourceTo(&source, target_pts_ns, max_blocks_per_source);
      if (!filled.Succeeded()) {
        if (complete != nullptr) {
          *complete = false;
        }
        return filled;
      }
      if (source.next_pts_ns + kContinuityToleranceNs < target_pts_ns) {
        all_complete = false;
      }
    }
    if (complete != nullptr) {
      *complete = all_complete;
    }
    return SinkFailure(CapturedPcmSinkStatus::Success, L"");
  }

  void CopyStatsTo(std::vector<AudioLiveCaptureEncodeSourceResult>* results) {
    if (results == nullptr) {
      return;
    }
    std::lock_guard lock(*mutex_);
    for (const auto& source : continuity_sources_) {
      if (source.result_index >= results->size()) {
        continue;
      }
      auto& result = (*results)[source.result_index];
      result.synthetic_silence_packet_count =
          source.synthetic_silence_packet_count;
      result.synthetic_silence_frame_count =
          source.synthetic_silence_frame_count;
      result.retimed_packet_count = source.retimed_packet_count;
    }
  }

 private:
  struct ContinuitySource {
    CapturedAudioSource source;
    PcmStreamFormat format;
    size_t result_index = 0;
    int64_t next_pts_ns = 0;
    uint64_t synthetic_silence_packet_count = 0;
    uint64_t synthetic_silence_frame_count = 0;
    uint64_t retimed_packet_count = 0;
  };

  static CapturedPcmSinkResult SinkFailure(CapturedPcmSinkStatus status,
                                           std::wstring message) {
    CapturedPcmSinkResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
  }

  ContinuitySource* FindSource(CapturedAudioSource source) noexcept {
    for (auto& candidate : continuity_sources_) {
      if (SameCapturedAudioSource(candidate.source, source)) {
        return &candidate;
      }
    }
    return nullptr;
  }

  CapturedPcmSinkResult FillSourceTo(ContinuitySource* source,
                                     int64_t target_pts_ns,
                                     size_t max_blocks) {
    size_t emitted = 0;
    while (source->next_pts_ns + kContinuityToleranceNs < target_pts_ns &&
           emitted < max_blocks) {
      const int64_t remaining_ns = target_pts_ns - source->next_pts_ns;
      const int64_t block_duration_ns =
          std::min(remaining_ns, kMaxSilenceBlockDurationNs);
      const long double exact_frames =
          (static_cast<long double>(block_duration_ns) *
           static_cast<long double>(source->format.sample_rate)) /
          1000000000.0L;
      const uint32_t frame_count = static_cast<uint32_t>(exact_frames);
      if (frame_count == 0) {
        break;
      }

      PcmPacketInfo silence;
      silence.silent = true;
      silence.timing.frame_count = frame_count;
      silence.timing.duration_ns =
          AudioFramesToNs(frame_count, source->format.sample_rate);
      silence.timing.qpc_position_ns =
          qpc_origin_ns_ + source->next_pts_ns;
      silence.timing.qpc_position_100ns = static_cast<uint64_t>(
          silence.timing.qpc_position_ns / 100);

      const auto delivered = bridge_->captured_pcm_sink()->OnCapturedPcm(
          CapturedPcmPacket{source->source, source->format, silence, {}});
      if (!delivered.Succeeded()) {
        return delivered;
      }

      source->next_pts_ns += silence.timing.duration_ns;
      ++source->synthetic_silence_packet_count;
      source->synthetic_silence_frame_count += frame_count;
      ++emitted;
    }
    return SinkFailure(CapturedPcmSinkStatus::Success, L"");
  }

  AudioCaptureEncodeBridge* bridge_ = nullptr;
  std::mutex* mutex_ = nullptr;
  int64_t qpc_origin_ns_ = 0;
  bool maintain_track_continuity_ = false;
  std::vector<ContinuitySource> continuity_sources_;
};

struct ActiveSource {
  size_t result_index = 0;
  std::unique_ptr<IAudioLiveCaptureSource> source;
};

}  // namespace

class AudioLiveCaptureEncodeSession::Impl final {
 public:
  Impl(AudioTrackPlan plan,
       AudioLiveCaptureEncodeOptions options,
       AudioEncodeSession* session,
       AudioLiveCaptureSourceFactory source_factory)
      : plan(std::move(plan)),
        options(options),
        session(session),
        source_factory(source_factory != nullptr
                           ? source_factory
                           : &CreateDefaultAudioLiveCaptureSource) {}

  void SnapshotBridge() {
    if (bridge == nullptr) {
      return;
    }
    live_result.sink_stats = bridge->sink_stats();
    live_result.last_dispatch = bridge->last_dispatch_result();
  }

  void SnapshotSources() {
    live_result.packet_count = 0;
    live_result.frame_count = 0;
    for (const auto& active : active_sources) {
      auto& source_result = live_result.sources[active.result_index];
      source_result.capture = active.source->SnapshotStats();
      live_result.packet_count += source_result.capture.packet_count;
      live_result.frame_count += source_result.capture.frame_count;
    }
    if (serialized_sink != nullptr) {
      serialized_sink->CopyStatsTo(&live_result.sources);
    }
  }

  AudioTrackPlan plan;
  AudioLiveCaptureEncodeOptions options;
  AudioEncodeSession* session = nullptr;
  AudioLiveCaptureSourceFactory source_factory = nullptr;
  std::unique_ptr<AudioCaptureEncodeBridge> bridge;
  std::unique_ptr<SerializedBridgeSink> serialized_sink;
  AudioCaptureManager manager;
  std::mutex bridge_mutex;
  std::vector<ActiveSource> active_sources;
  AudioLiveCaptureEncodeResult live_result;
  bool prepared = false;
  bool running = false;
  bool sources_stopped = true;
  bool queues_drained = false;
  bool encoders_flushed = false;
};

bool AudioLiveCaptureEncodeRunResult::Succeeded() const noexcept {
  return status == AudioLiveCaptureEncodeStatus::Success;
}

std::unique_ptr<IAudioLiveCaptureSource> CreateDefaultAudioLiveCaptureSource(
    const AudioCaptureSourceBinding& binding) {
  if (binding.runtime != AudioCaptureSourceRuntime::SystemLoopback &&
      binding.runtime != AudioCaptureSourceRuntime::Microphone) {
    return nullptr;
  }

  return std::make_unique<WasapiLiveCaptureSource>(binding.source);
}

AudioLiveCaptureEncodeSession::AudioLiveCaptureEncodeSession(
    AudioTrackPlan plan,
    AudioLiveCaptureEncodeOptions options,
    AudioEncodeSession* session,
    AudioLiveCaptureSourceFactory source_factory)
    : impl_(std::make_unique<Impl>(std::move(plan), options, session,
                                  source_factory)) {}

AudioLiveCaptureEncodeSession::~AudioLiveCaptureEncodeSession() {
  if (impl_ == nullptr || !impl_->prepared) {
    return;
  }
  (void)StopSources();
  (void)DrainQueuedBlocks();
  (void)FlushEncoders();
}

AudioLiveCaptureEncodeRunResult AudioLiveCaptureEncodeSession::Prepare() {
  if (impl_ == nullptr || !SessionOptionsAreValid(impl_->options) ||
      impl_->session == nullptr || !impl_->session->IsConfigured() ||
      impl_->source_factory == nullptr) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode session configuration is "
                  L"invalid.");
  }
  if (impl_->prepared) {
    return Result(AudioLiveCaptureEncodeStatus::Success, L"");
  }

  impl_->live_result = {};
  impl_->bridge = std::make_unique<AudioCaptureEncodeBridge>(
      impl_->plan, impl_->session, impl_->options.qpc_origin_ns);
  impl_->live_result.bridge_configured = impl_->bridge->IsConfigured();
  if (!impl_->bridge->IsConfigured()) {
    impl_->bridge.reset();
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode needs a configured bridge.");
  }

  impl_->serialized_sink = std::make_unique<SerializedBridgeSink>(
      impl_->bridge.get(), &impl_->bridge_mutex,
      impl_->options.qpc_origin_ns,
      impl_->options.maintain_track_continuity);
  const auto configured =
      impl_->manager.Configure(impl_->plan, impl_->serialized_sink.get());
  if (!configured.Succeeded()) {
    impl_->serialized_sink.reset();
    impl_->bridge.reset();
    return Result(StatusFromManager(configured.status), configured.message);
  }

  impl_->live_result.deferred_mixed_track =
      impl_->manager.has_deferred_mixed_track();
  size_t result_index = 0;
  for (const auto& binding : impl_->manager.sources()) {
    impl_->live_result.sources.push_back(SourceResultFromBinding(binding));
    if (binding.support == AudioCaptureSourceSupport::Deferred) {
      ++impl_->live_result.deferred_source_count;
      impl_->live_result.sources.back().message =
          std::wstring(AudioCaptureSourceRuntimeName(binding.runtime)) +
          L" capture is deferred.";
    } else if (impl_->options.maintain_track_continuity) {
      PcmStreamFormat format;
      if (!impl_->session->TryGetTrackInputFormat(binding.track_id, &format) ||
          !impl_->serialized_sink->AddSource(binding.source, format,
                                             result_index)) {
        impl_->manager.Reset();
        impl_->serialized_sink.reset();
        impl_->bridge.reset();
        return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                      L"Live audio continuity could not resolve a capture "
                      L"track input format.");
      }
    }
    ++result_index;
  }

  impl_->prepared = true;
  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

AudioLiveCaptureEncodeRunResult AudioLiveCaptureEncodeSession::Start() {
  if (impl_ == nullptr || !impl_->prepared || impl_->running ||
      !impl_->sources_stopped) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode session cannot start in its "
                  L"current state.");
  }

  impl_->active_sources.clear();
  impl_->queues_drained = false;
  impl_->encoders_flushed = false;
  impl_->sources_stopped = false;

  size_t result_index = 0;
  for (const auto& binding : impl_->manager.sources()) {
    auto& source_result = impl_->live_result.sources[result_index++];
    if (binding.support == AudioCaptureSourceSupport::Deferred) {
      continue;
    }

    source_result.attempted = true;
    ++impl_->live_result.attempted_source_count;
    auto live_source =
        impl_->source_factory == &CreateDefaultAudioLiveCaptureSource
            ? std::make_unique<WasapiLiveCaptureSource>(
                  binding.source, impl_->options.performance_mode)
            : impl_->source_factory(binding);
    if (live_source == nullptr) {
      source_result.start_result = SourceResult(
          WasapiCaptureSourceStatus::InvalidConfig,
          L"Live capture source factory returned no source.");
    } else {
      source_result.start_result =
          live_source->Start(impl_->serialized_sink.get());
    }
    source_result.started = source_result.start_result.Succeeded();
    if (!source_result.started) {
      source_result.message = source_result.start_result.message;
      const std::wstring failure =
          SourceLabel(binding) + L" failed to start: " +
          source_result.message;
      (void)StopSources();
      (void)DrainQueuedBlocks();
      (void)FlushEncoders();
      return Result(AudioLiveCaptureEncodeStatus::SourceStartFailed,
                    failure);
    }

    ++impl_->live_result.started_source_count;
    impl_->active_sources.push_back(
        ActiveSource{result_index - 1, std::move(live_source)});
  }

  if (impl_->active_sources.empty()) {
    impl_->sources_stopped = true;
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode has no supported source to "
                  L"start.");
  }

  impl_->running = true;
  impl_->SnapshotSources();
  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

AudioLiveCaptureEncodeRunResult AudioLiveCaptureEncodeSession::DrainTick() {
  if (impl_ == nullptr || !impl_->running || impl_->bridge == nullptr) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode session is not running.");
  }

  if (impl_->options.maintain_track_continuity) {
    int64_t current_pts_ns = 0;
    std::wstring clock_error;
    if (!CurrentSessionPtsNs(impl_->options.qpc_origin_ns, &current_pts_ns,
                             &clock_error)) {
      return Result(AudioLiveCaptureEncodeStatus::DrainFailed,
                    clock_error.empty()
                        ? L"Could not query the audio continuity clock."
                        : std::move(clock_error));
    }
    const int64_t target_pts_ns =
        std::max<int64_t>(0, current_pts_ns - kContinuityLagNs);
    bool complete = false;
    const auto filled = impl_->serialized_sink->FillContinuityTo(
        target_pts_ns, kMaxSilenceBlocksPerTick, &complete);
    if (!filled.Succeeded()) {
      return Result(AudioLiveCaptureEncodeStatus::DrainFailed,
                    filled.message.empty()
                        ? L"Could not queue continuity silence."
                        : filled.message);
    }
  }

  {
    std::lock_guard lock(impl_->bridge_mutex);
    impl_->live_result.last_tick_drain = impl_->bridge->DrainQueuedBlocks(
        impl_->options.max_blocks_per_drain_tick);
    impl_->live_result.drained_block_count +=
        impl_->live_result.last_tick_drain.processed_block_count;
    impl_->SnapshotBridge();
  }
  if (!impl_->live_result.last_tick_drain.Succeeded()) {
    return Result(AudioLiveCaptureEncodeStatus::DrainFailed,
                  impl_->live_result.last_tick_drain.message);
  }
  ++impl_->live_result.drain_tick_count;

  for (const auto& active : impl_->active_sources) {
    if (active.source->IsRunning()) {
      continue;
    }
    const auto last = active.source->LastResult();
    if (!last.Succeeded()) {
      return Result(AudioLiveCaptureEncodeStatus::CaptureFailed,
                    last.message);
    }
  }
  impl_->SnapshotSources();

  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

AudioLiveCaptureEncodeRunResult
AudioLiveCaptureEncodeSession::StopSources() {
  if (impl_ == nullptr || !impl_->prepared) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode session is not prepared.");
  }
  if (impl_->sources_stopped) {
    return Result(AudioLiveCaptureEncodeStatus::Success, L"");
  }

  bool failed = false;
  std::wstring first_failure;
  for (auto& active : impl_->active_sources) {
    active.source->Stop();
    auto& source_result = impl_->live_result.sources[active.result_index];
    source_result.stopped = true;
    source_result.final_result = active.source->LastResult();

    if (!source_result.final_result.Succeeded()) {
      failed = true;
      source_result.message = source_result.final_result.message;
      if (first_failure.empty()) {
        first_failure = std::wstring(
                            AudioCaptureSourceRuntimeName(source_result.runtime)) +
                        L" track " + std::to_wstring(source_result.track_id) +
                        L" failed while running: " + source_result.message;
      }
    }
  }
  impl_->SnapshotSources();

  if (impl_->options.maintain_track_continuity && !failed) {
    int64_t current_pts_ns = 0;
    std::wstring clock_error;
    if (!CurrentSessionPtsNs(impl_->options.qpc_origin_ns, &current_pts_ns,
                             &clock_error)) {
      failed = true;
      first_failure = clock_error.empty()
                          ? L"Could not query the final audio continuity clock."
                          : std::move(clock_error);
    } else {
      bool complete = false;
      const auto filled = impl_->serialized_sink->FillContinuityTo(
          current_pts_ns, kMaxFinalSilenceBlocks, &complete);
      if (!filled.Succeeded() || !complete) {
        failed = true;
        first_failure = !filled.message.empty()
                            ? filled.message
                            : L"Could not complete final audio continuity "
                              L"silence without overflowing the queue.";
      }
    }
    impl_->SnapshotSources();
  }

  impl_->active_sources.clear();
  impl_->running = false;
  impl_->sources_stopped = true;
  return failed
             ? Result(AudioLiveCaptureEncodeStatus::CaptureFailed,
                      std::move(first_failure))
             : Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

AudioLiveCaptureEncodeRunResult
AudioLiveCaptureEncodeSession::DrainQueuedBlocks() {
  if (impl_ == nullptr || !impl_->prepared || !impl_->sources_stopped ||
      impl_->bridge == nullptr) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode must stop sources before final "
                  L"queue draining.");
  }
  if (impl_->queues_drained) {
    return Result(AudioLiveCaptureEncodeStatus::Success, L"");
  }

  {
    std::lock_guard lock(impl_->bridge_mutex);
    impl_->live_result.final_drain =
        impl_->bridge->DrainAllQueuedBlocks();
    impl_->live_result.drained_block_count +=
        impl_->live_result.final_drain.processed_block_count;
    impl_->SnapshotBridge();
  }
  if (!impl_->live_result.final_drain.Succeeded()) {
    return Result(AudioLiveCaptureEncodeStatus::DrainFailed,
                  impl_->live_result.final_drain.message);
  }

  impl_->queues_drained = true;
  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

AudioLiveCaptureEncodeRunResult
AudioLiveCaptureEncodeSession::FlushEncoders() {
  if (impl_ == nullptr || !impl_->prepared || !impl_->queues_drained ||
      impl_->session == nullptr) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode must drain queued PCM before "
                  L"flushing AAC encoders.");
  }
  if (impl_->encoders_flushed) {
    return Result(AudioLiveCaptureEncodeStatus::Success, L"");
  }

  {
    std::lock_guard lock(impl_->bridge_mutex);
    impl_->live_result.flush = impl_->session->FlushAllTracks();
    impl_->SnapshotBridge();
  }
  if (!impl_->live_result.flush.Succeeded()) {
    return Result(AudioLiveCaptureEncodeStatus::FlushFailed,
                  impl_->live_result.flush.message);
  }

  impl_->encoders_flushed = true;
  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

bool AudioLiveCaptureEncodeSession::IsPrepared() const noexcept {
  return impl_ != nullptr && impl_->prepared;
}

bool AudioLiveCaptureEncodeSession::IsRunning() const noexcept {
  return impl_ != nullptr && impl_->running;
}

const AudioLiveCaptureEncodeResult&
AudioLiveCaptureEncodeSession::result() const noexcept {
  return impl_->live_result;
}

AudioLiveCaptureEncodeRunResult RunAudioLiveCaptureEncode(
    const AudioTrackPlan& plan,
    const AudioLiveCaptureEncodeOptions& options,
    AudioEncodeSession* session,
    AudioLiveCaptureEncodeResult* result) {
  return RunAudioLiveCaptureEncode(plan, options, session,
                                   &CreateDefaultAudioLiveCaptureSource,
                                   result);
}

AudioLiveCaptureEncodeRunResult RunAudioLiveCaptureEncode(
    const AudioTrackPlan& plan,
    const AudioLiveCaptureEncodeOptions& options,
    AudioEncodeSession* session,
    AudioLiveCaptureSourceFactory source_factory,
    AudioLiveCaptureEncodeResult* result) {
  if (result == nullptr) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode needs an output destination.");
  }
  *result = {};

  if (!RunOptionsAreValid(options)) {
    return Result(AudioLiveCaptureEncodeStatus::InvalidConfig,
                  L"Live audio capture encode options are invalid.");
  }

  AudioLiveCaptureEncodeSession live(plan, options, session,
                                     source_factory);
  auto step = live.Prepare();
  if (!step.Succeeded()) {
    *result = live.result();
    return step;
  }

  step = live.Start();
  if (!step.Succeeded()) {
    *result = live.result();
    return step;
  }

  AudioLiveCaptureEncodeRunResult run_failure =
      Result(AudioLiveCaptureEncodeStatus::Success, L"");
  const auto deadline = std::chrono::steady_clock::now() + options.duration;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = deadline - now;
    const auto sleep_duration =
        std::min(options.drain_interval,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     remaining));
    if (sleep_duration > std::chrono::milliseconds(0)) {
      std::this_thread::sleep_for(sleep_duration);
    }

    step = live.DrainTick();
    if (!step.Succeeded()) {
      run_failure = step;
      break;
    }
  }

  const auto stopped = live.StopSources();
  const auto drained = live.DrainQueuedBlocks();
  const auto flushed = live.FlushEncoders();
  *result = live.result();

  if (!drained.Succeeded()) {
    return drained;
  }
  if (!flushed.Succeeded()) {
    return flushed;
  }
  if (!run_failure.Succeeded()) {
    return run_failure;
  }
  if (!stopped.Succeeded()) {
    return stopped;
  }
  return Result(AudioLiveCaptureEncodeStatus::Success, L"");
}

const wchar_t* AudioLiveCaptureEncodeStatusName(
    AudioLiveCaptureEncodeStatus status) noexcept {
  switch (status) {
    case AudioLiveCaptureEncodeStatus::Success:
      return L"success";
    case AudioLiveCaptureEncodeStatus::InvalidConfig:
      return L"invalid config";
    case AudioLiveCaptureEncodeStatus::SourceStartFailed:
      return L"source start failed";
    case AudioLiveCaptureEncodeStatus::CaptureFailed:
      return L"capture failed";
    case AudioLiveCaptureEncodeStatus::DrainFailed:
      return L"drain failed";
    case AudioLiveCaptureEncodeStatus::FlushFailed:
      return L"flush failed";
  }
  return L"unknown";
}

}  // namespace olouie::audio
