#include "record/Mp4Muxer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if OLOUIE_FFMPEG_CONFIGURED
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace olouie::record {
namespace {

Mp4MuxResult Result(Mp4MuxStatus status, std::wstring message,
                    DiskWriteFault write_fault = {}) {
  Mp4MuxResult result;
  result.status = status;
  result.message = std::move(message);
  result.write_fault = std::move(write_fault);
  return result;
}

bool IsMp4Path(const std::filesystem::path& path) {
  return path.extension() == L".mp4" || path.extension() == L".MP4";
}

const MuxTrack* FindSingleVideoTrack(const MuxPlan& plan) {
  const MuxTrack* video_track = nullptr;
  for (const auto& track : plan.tracks) {
    if (track.codec_id != CodecId::H264) {
      continue;
    }

    if (video_track != nullptr) {
      return nullptr;
    }
    video_track = &track;
  }
  return video_track;
}

const MuxPacketRef* FindFirstVideoPacket(const MuxPlan& plan,
                                         uint32_t video_track_id) {
  const auto found = std::find_if(
      plan.packets.begin(), plan.packets.end(),
      [video_track_id](const MuxPacketRef& packet) {
        return packet.packet.metadata.track_id == video_track_id &&
               packet.packet.metadata.codec_id == CodecId::H264;
      });
  return found == plan.packets.end() ? nullptr : &(*found);
}

Mp4H264VideoTrack BuildMp4VideoTrack(
    const VideoExportTrackMetadata& video) {
  Mp4H264VideoTrack track;
  track.track_id = video.track_id;
  track.codec_id = video.codec_id;
  track.width = video.width;
  track.height = video.height;
  track.fps_numerator = video.fps_numerator;
  track.fps_denominator = video.fps_denominator;
  track.packet_format = video.h264_packet_format;
  track.sps = video.h264_sps;
  track.pps = video.h264_pps;
  track.avcc_extradata = video.h264_avcc_extradata;
  return track;
}

Mp4AacAudioTrack BuildMp4AudioTrack(
    const AudioExportTrackMetadata& audio) {
  Mp4AacAudioTrack track;
  track.track_id = audio.track_id;
  track.codec_id = audio.codec_id;
  track.source_kind = audio.source_kind;
  track.name = audio.name;
  track.sample_rate = audio.sample_rate;
  track.channel_count = audio.channel_count;
  track.bitrate_bps = audio.bitrate_bps;
  track.frame_samples = audio.aac_frame_samples;
  track.payload_type = audio.aac_payload_type;
  track.profile_level_indication = audio.aac_profile_level_indication;
  track.audio_object_type = audio.aac_audio_object_type;
  track.audio_specific_config = audio.aac_audio_specific_config;
  track.encoder_name = audio.encoder_name;
  return track;
}

bool AddU64(uint64_t left, uint64_t right, uint64_t* output) {
  if (output == nullptr ||
      right > std::numeric_limits<uint64_t>::max() - left) {
    return false;
  }

  *output = left + right;
  return true;
}

bool AddPayloadBytes(uint64_t bytes, uint64_t* total) {
  if (total == nullptr ||
      bytes > std::numeric_limits<uint64_t>::max() - *total) {
    return false;
  }

  *total += bytes;
  return true;
}

size_t AnnexBStartCodeLength(const std::vector<uint8_t>& data,
                             size_t offset) noexcept {
  if (offset + 3u <= data.size() && data[offset] == 0 &&
      data[offset + 1u] == 0 && data[offset + 2u] == 1) {
    return 3u;
  }

  if (offset + 4u <= data.size() && data[offset] == 0 &&
      data[offset + 1u] == 0 && data[offset + 2u] == 0 &&
      data[offset + 3u] == 1) {
    return 4u;
  }

  return 0;
}

size_t FindNextAnnexBStartCode(const std::vector<uint8_t>& data,
                               size_t offset) noexcept {
  while (offset < data.size()) {
    if (AnnexBStartCodeLength(data, offset) != 0) {
      return offset;
    }
    ++offset;
  }

  return data.size();
}

bool AppendU32Be(uint32_t value, std::vector<uint8_t>* output) {
  if (output == nullptr ||
      output->size() > std::numeric_limits<size_t>::max() - 4u) {
    return false;
  }

  output->push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
  output->push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
  output->push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
  output->push_back(static_cast<uint8_t>(value & 0xFFu));
  return true;
}

bool ConvertAnnexBToLengthPrefixedSample(
    const std::vector<uint8_t>& annex_b,
    std::vector<uint8_t>* sample,
    std::wstring* error) {
  if (sample == nullptr) {
    if (error != nullptr) {
      *error = L"MP4 sample conversion needs a destination buffer.";
    }
    return false;
  }

  sample->clear();
  if (annex_b.empty()) {
    if (error != nullptr) {
      *error = L"MP4 sample conversion needs Annex B payload bytes.";
    }
    return false;
  }

  size_t offset = 0;
  while (offset < annex_b.size()) {
    const size_t start_code_length = AnnexBStartCodeLength(annex_b, offset);
    if (start_code_length == 0) {
      if (sample->empty() && annex_b[offset] == 0) {
        ++offset;
        continue;
      }

      if (error != nullptr) {
        *error = L"MP4 sample conversion found payload bytes outside Annex B "
                 L"start codes.";
      }
      return false;
    }

    const size_t nal_start = offset + start_code_length;
    if (nal_start >= annex_b.size()) {
      if (error != nullptr) {
        *error = L"MP4 sample conversion found an empty H.264 NAL unit.";
      }
      return false;
    }

    const size_t next_start_code =
        FindNextAnnexBStartCode(annex_b, nal_start + 1u);
    size_t nal_end = next_start_code;
    while (nal_end > nal_start && annex_b[nal_end - 1u] == 0) {
      --nal_end;
    }

    if (nal_end <= nal_start) {
      if (error != nullptr) {
        *error = L"MP4 sample conversion found an empty H.264 NAL unit.";
      }
      return false;
    }

    const size_t nal_size = nal_end - nal_start;
    if (nal_size > std::numeric_limits<uint32_t>::max()) {
      if (error != nullptr) {
        *error = L"MP4 sample conversion H.264 NAL unit is too large.";
      }
      return false;
    }

    if (!AppendU32Be(static_cast<uint32_t>(nal_size), sample) ||
        sample->size() > std::numeric_limits<size_t>::max() - nal_size) {
      if (error != nullptr) {
        *error = L"MP4 sample conversion output size overflowed.";
      }
      return false;
    }

    sample->insert(sample->end(), annex_b.begin() + nal_start,
                   annex_b.begin() + nal_end);
    offset = next_start_code;
  }

  if (sample->empty()) {
    if (error != nullptr) {
      *error = L"MP4 sample conversion did not find H.264 NAL units.";
    }
    return false;
  }

  return true;
}

bool ReadPacketPayload(std::ifstream* input,
                       const PacketIndexEntry& entry,
                       std::vector<uint8_t>* payload,
                       std::wstring* error);

bool ValidatePacketWriterRequest(const Mp4MuxRequest& request,
                                 std::wstring* error);

#if OLOUIE_FFMPEG_CONFIGURED
bool FitsInt(uint32_t value) {
  return value <=
         static_cast<uint32_t>(std::numeric_limits<int>::max());
}

std::wstring WidenAscii(std::string_view text) {
  std::wstring wide;
  wide.reserve(text.size());
  for (char character : text) {
    wide.push_back(static_cast<unsigned char>(character));
  }
  return wide;
}

std::wstring FfmpegError(std::wstring prefix, int error_code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
  if (av_strerror(error_code, message.data(), message.size()) == 0) {
    prefix += L": ";
    prefix += WidenAscii(message.data());
  }
  return prefix;
}

std::string PathToUtf8(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  std::string converted;
  converted.reserve(utf8.size());
  for (const char8_t character : utf8) {
    converted.push_back(static_cast<char>(character));
  }
  return converted;
}

Mp4MuxResult ConfigureFfmpegH264VideoStream(
    AVFormatContext* context,
    const Mp4MuxRequest& request,
    AVStream** stream,
    Mp4MuxStreamSetupStats* stats) {
  if (context == nullptr || stream == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"FFmpeg MP4 video stream setup needs a format context.");
  }

  *stream = nullptr;
  if (!FitsInt(request.video_track.track_id) ||
      !FitsInt(request.video_track.width) ||
      !FitsInt(request.video_track.height) ||
      !FitsInt(request.video_track.fps_numerator) ||
      !FitsInt(request.video_track.fps_denominator)) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 stream setup video metadata exceeds FFmpeg integer "
                  L"limits.");
  }

  const std::size_t padding =
      static_cast<std::size_t>(AV_INPUT_BUFFER_PADDING_SIZE);
  if (request.video_track.avcc_extradata.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      request.video_track.avcc_extradata.size() >
          std::numeric_limits<std::size_t>::max() - padding) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 stream setup H.264 extradata is too large.");
  }

  AVStream* video_stream = avformat_new_stream(context, nullptr);
  if (video_stream == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  L"FFmpeg could not allocate an MP4 video stream.");
  }

  video_stream->id = static_cast<int>(request.video_track.track_id);
  video_stream->time_base = AVRational{1, 90000};
  video_stream->avg_frame_rate =
      AVRational{static_cast<int>(request.video_track.fps_numerator),
                 static_cast<int>(request.video_track.fps_denominator)};
  video_stream->r_frame_rate = video_stream->avg_frame_rate;

  AVCodecParameters* parameters = video_stream->codecpar;
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->width = static_cast<int>(request.video_track.width);
  parameters->height = static_cast<int>(request.video_track.height);
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->extradata_size =
      static_cast<int>(request.video_track.avcc_extradata.size());
  parameters->extradata =
      static_cast<uint8_t*>(av_mallocz(request.video_track.avcc_extradata.size() +
                                       padding));
  if (parameters->extradata == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  L"FFmpeg could not allocate H.264 extradata.");
  }
  std::memcpy(parameters->extradata,
              request.video_track.avcc_extradata.data(),
              request.video_track.avcc_extradata.size());

  if (stats != nullptr) {
    stats->output_context_allocated = true;
    stats->video_stream_created = true;
    stats->h264_parameters_applied = true;
    stats->video_track_id = request.video_track.track_id;
    stats->width = request.video_track.width;
    stats->height = request.video_track.height;
    stats->fps_numerator = request.video_track.fps_numerator;
    stats->fps_denominator = request.video_track.fps_denominator;
    stats->extradata_bytes = request.video_track.avcc_extradata.size();
  }

  *stream = video_stream;
  return Result(Mp4MuxStatus::Success,
                L"FFmpeg MP4 video stream setup validation succeeded.");
}

Mp4MuxResult ConfigureFfmpegAacAudioStream(
    AVFormatContext* context,
    const Mp4AacAudioTrack& audio,
    AVStream** stream,
    Mp4MuxStreamSetupStats* stats) {
  if (context == nullptr || stream == nullptr || !audio.IsReady() ||
      !FitsInt(audio.track_id) || !FitsInt(audio.sample_rate) ||
      !FitsInt(audio.channel_count) || !FitsInt(audio.bitrate_bps) ||
      !FitsInt(audio.frame_samples)) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"FFmpeg MP4 AAC stream setup needs valid audio metadata.");
  }
  *stream = nullptr;

  const size_t padding = static_cast<size_t>(AV_INPUT_BUFFER_PADDING_SIZE);
  if (audio.audio_specific_config.size() >
          static_cast<size_t>(std::numeric_limits<int>::max()) ||
      audio.audio_specific_config.size() >
          std::numeric_limits<size_t>::max() - padding) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 stream setup AAC decoder configuration is too "
                  L"large.");
  }

  AVStream* audio_stream = avformat_new_stream(context, nullptr);
  if (audio_stream == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  L"FFmpeg could not allocate an MP4 audio stream.");
  }

  audio_stream->id = static_cast<int>(audio.track_id);
  audio_stream->time_base =
      AVRational{1, static_cast<int>(audio.sample_rate)};

  AVCodecParameters* parameters = audio_stream->codecpar;
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->profile = AV_PROFILE_AAC_LOW;
  parameters->sample_rate = static_cast<int>(audio.sample_rate);
  parameters->bit_rate = static_cast<int64_t>(audio.bitrate_bps);
  parameters->frame_size = static_cast<int>(audio.frame_samples);
  av_channel_layout_default(&parameters->ch_layout,
                            static_cast<int>(audio.channel_count));
  parameters->extradata_size =
      static_cast<int>(audio.audio_specific_config.size());
  parameters->extradata = static_cast<uint8_t*>(
      av_mallocz(audio.audio_specific_config.size() + padding));
  if (parameters->extradata == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  L"FFmpeg could not allocate AAC decoder configuration.");
  }
  std::memcpy(parameters->extradata, audio.audio_specific_config.data(),
              audio.audio_specific_config.size());

  if (!audio.name.empty()) {
    const auto title = PathToUtf8(std::filesystem::path(audio.name));
    av_dict_set(&audio_stream->metadata, "title", title.c_str(), 0);
    av_dict_set(&audio_stream->metadata, "handler_name", title.c_str(), 0);
  }

  if (stats != nullptr) {
    ++stats->audio_stream_count;
    ++stats->aac_parameters_applied_count;
    stats->audio_extradata_bytes += audio.audio_specific_config.size();
  }
  *stream = audio_stream;
  return Result(Mp4MuxStatus::Success,
                L"FFmpeg MP4 AAC stream setup validation succeeded.");
}

Mp4MuxResult ConfigureFfmpegStreams(
    AVFormatContext* context,
    const Mp4MuxRequest& request,
    std::map<uint32_t, AVStream*>* streams,
    Mp4MuxStreamSetupStats* stats) {
  if (streams == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"FFmpeg MP4 stream setup needs a stream map.");
  }
  streams->clear();

  AVStream* video_stream = nullptr;
  auto setup = ConfigureFfmpegH264VideoStream(
      context, request, &video_stream, stats);
  if (!setup.Succeeded()) {
    return setup;
  }
  streams->emplace(request.video_track.track_id, video_stream);

  for (const auto& audio : request.audio_tracks) {
    AVStream* audio_stream = nullptr;
    setup = ConfigureFfmpegAacAudioStream(context, audio, &audio_stream,
                                          stats);
    if (!setup.Succeeded()) {
      return setup;
    }
    if (!streams->emplace(audio.track_id, audio_stream).second) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 stream setup found duplicate track ids.");
    }
  }

  return Result(Mp4MuxStatus::Success,
                L"FFmpeg MP4 stream setup validation succeeded.");
}

Mp4MuxResult WriteFfmpegMp4(const Mp4MuxRequest& request) {
  std::wstring writer_error;
  if (!ValidatePacketWriterRequest(request, &writer_error)) {
    return Result(Mp4MuxStatus::InvalidRequest, std::move(writer_error));
  }

  std::error_code fs_error;
  const bool final_exists =
      std::filesystem::exists(request.final_output_path, fs_error);
  if (fs_error) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux, DiskWriteOperation::InspectPath,
        request.final_output_path, fs_error);
    return Result(Mp4MuxStatus::FileSystemError,
                  DescribeDiskWriteFault(fault), fault);
  }
  if (final_exists && !request.allow_overwrite) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux,
        DiskWriteOperation::AtomicPublish, request.final_output_path,
        std::make_error_code(std::errc::file_exists));
    return Result(Mp4MuxStatus::DestinationExists,
                  DescribeDiskWriteFault(fault), fault);
  }

  const auto temp_parent = request.temp_output_path.parent_path();
  if (!temp_parent.empty()) {
    std::filesystem::create_directories(temp_parent, fs_error);
    if (fs_error) {
      auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::Mp4Mux,
          DiskWriteOperation::CreateDirectories, temp_parent, fs_error);
      return Result(Mp4MuxStatus::FileSystemError,
                    DescribeDiskWriteFault(fault), fault);
    }
  }

  std::filesystem::remove(request.temp_output_path, fs_error);
  if (fs_error) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux,
        DiskWriteOperation::RemoveStalePartial, request.temp_output_path,
        fs_error);
    return Result(Mp4MuxStatus::FileSystemError,
                  DescribeDiskWriteFault(fault), fault);
  }

  auto with_temp_cleanup = [&](Mp4MuxResult result) {
    fs_error.clear();
    std::filesystem::remove(request.temp_output_path, fs_error);
    if (fs_error) {
      auto cleanup_fault = MakeDiskWriteFault(
          DiskWriteSubsystem::Mp4Mux,
          DiskWriteOperation::RemoveFailedPartial,
          request.temp_output_path, fs_error);
      if (!result.message.empty()) {
        result.message += L" Cleanup: ";
      }
      result.message += DescribeDiskWriteFault(cleanup_fault);
      if (!result.write_fault.Failed()) {
        result.write_fault = std::move(cleanup_fault);
      }
    }
    return result;
  };

  const std::string temp_path = PathToUtf8(request.temp_output_path);
  AVFormatContext* context = nullptr;
  int ffmpeg_result =
      avformat_alloc_output_context2(&context, nullptr, "mp4",
                                     temp_path.c_str());
  if (ffmpeg_result < 0 || context == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  FfmpegError(L"FFmpeg could not allocate an MP4 output "
                              L"context",
                              ffmpeg_result));
  }

  auto cleanup_context = [&context]() {
    int close_result = 0;
    if (context != nullptr) {
      if (context->pb != nullptr) {
        close_result = avio_closep(&context->pb);
      }
      avformat_free_context(context);
      context = nullptr;
    }
    return close_result;
  };

  std::map<uint32_t, AVStream*> streams;
  auto setup_result =
      ConfigureFfmpegStreams(context, request, &streams, nullptr);
  if (!setup_result.Succeeded()) {
    (void)cleanup_context();
    return with_temp_cleanup(std::move(setup_result));
  }

  if ((context->oformat->flags & AVFMT_NOFILE) == 0) {
    ffmpeg_result =
        avio_open(&context->pb, temp_path.c_str(), AVIO_FLAG_WRITE);
    if (ffmpeg_result < 0) {
      (void)cleanup_context();
      auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::Mp4Mux,
          DiskWriteOperation::OpenTemporaryFile,
          request.temp_output_path, {}, ffmpeg_result,
          FfmpegError(L"FFmpeg could not open the temporary MP4 output",
                      ffmpeg_result));
      return with_temp_cleanup(Result(Mp4MuxStatus::FileSystemError,
                                      DescribeDiskWriteFault(fault),
                                      fault));
    }
  }

  ffmpeg_result = avformat_write_header(context, nullptr);
  if (ffmpeg_result < 0) {
    (void)cleanup_context();
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux, DiskWriteOperation::WriteHeader,
        request.temp_output_path, {}, ffmpeg_result,
        FfmpegError(L"FFmpeg could not write the MP4 header",
                    ffmpeg_result));
    return with_temp_cleanup(Result(Mp4MuxStatus::FileSystemError,
                                    DescribeDiskWriteFault(fault),
                                    fault));
  }

  std::ifstream input(request.packet_file_path, std::ios::binary);
  if (!input.is_open()) {
    (void)cleanup_context();
    return with_temp_cleanup(Result(
        Mp4MuxStatus::FileSystemError,
        L"MP4 mux could not open packets.dat at '" +
            request.packet_file_path.wstring() + L"'."));
  }

  const AVRational ns_time_base{1, 1000000000};
  std::vector<uint8_t> stored_payload;
  std::vector<uint8_t> mp4_sample;
  for (const auto& packet_ref : request.plan.packets) {
    const auto& metadata = packet_ref.packet.metadata;
    const auto stream_found = streams.find(metadata.track_id);
    if (stream_found == streams.end()) {
      (void)cleanup_context();
      return with_temp_cleanup(Result(
          Mp4MuxStatus::InvalidRequest,
          L"MP4 mux packet references an unconfigured stream."));
    }

    std::wstring read_error;
    if (!ReadPacketPayload(&input, packet_ref.packet, &stored_payload,
                           &read_error)) {
      (void)cleanup_context();
      return with_temp_cleanup(
          Result(Mp4MuxStatus::FileSystemError, std::move(read_error)));
    }

    std::vector<uint8_t>* sample = &stored_payload;
    if (metadata.codec_id == CodecId::H264) {
      std::wstring conversion_error;
      if (!ConvertAnnexBToLengthPrefixedSample(
              stored_payload, &mp4_sample, &conversion_error)) {
        (void)cleanup_context();
        return with_temp_cleanup(Result(Mp4MuxStatus::InvalidRequest,
                                        std::move(conversion_error)));
      }
      sample = &mp4_sample;
    }

    if (sample->empty() || sample->size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      (void)cleanup_context();
      return with_temp_cleanup(Result(
          Mp4MuxStatus::InvalidRequest,
          L"MP4 mux packet payload size is invalid."));
    }

    AVStream* stream = stream_found->second;
    AVPacket packet{};
    packet.stream_index = stream->index;
    packet.data = sample->data();
    packet.size = static_cast<int>(sample->size());
    packet.pts = packet_ref.output_pts_ns;
    packet.dts = packet_ref.output_dts_ns;
    packet.duration = packet_ref.duration_ns;
    if (packet_ref.packet.IsKeyframe()) {
      packet.flags |= AV_PKT_FLAG_KEY;
    }
    av_packet_rescale_ts(&packet, ns_time_base, stream->time_base);

    ffmpeg_result = av_interleaved_write_frame(context, &packet);
    if (ffmpeg_result < 0) {
      (void)cleanup_context();
      auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::Mp4Mux, DiskWriteOperation::WritePacket,
          request.temp_output_path, {}, ffmpeg_result,
          FfmpegError(L"FFmpeg could not write an MP4 packet",
                      ffmpeg_result));
      return with_temp_cleanup(Result(Mp4MuxStatus::FileSystemError,
                                      DescribeDiskWriteFault(fault),
                                      fault));
    }
  }

  ffmpeg_result = av_write_trailer(context);
  if (ffmpeg_result < 0) {
    (void)cleanup_context();
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux, DiskWriteOperation::WriteTrailer,
        request.temp_output_path, {}, ffmpeg_result,
        FfmpegError(L"FFmpeg could not finalize the MP4 trailer",
                    ffmpeg_result));
    return with_temp_cleanup(Result(Mp4MuxStatus::FileSystemError,
                                    DescribeDiskWriteFault(fault),
                                    fault));
  }

  const int close_result = cleanup_context();
  if (close_result < 0) {
    auto fault = MakeDiskWriteFault(
        DiskWriteSubsystem::Mp4Mux, DiskWriteOperation::Close,
        request.temp_output_path, {}, close_result,
        FfmpegError(L"FFmpeg could not close the temporary MP4 output",
                    close_result));
    return with_temp_cleanup(Result(Mp4MuxStatus::FileSystemError,
                                    DescribeDiskWriteFault(fault),
                                    fault));
  }
  const auto rename_result = Mp4Muxer::AtomicRename(
      request.temp_output_path, request.final_output_path,
      request.allow_overwrite);
  if (!rename_result.Succeeded()) {
    auto preserved = rename_result;
    preserved.message +=
        L" The complete temporary MP4 remains at '" +
        request.temp_output_path.wstring() + L"'.";
    return preserved;
  }

  return Result(Mp4MuxStatus::Success, L"MP4 mux completed.");
}
#endif

bool ReadPayloadBytes(std::ifstream* input,
                      uint64_t payload_offset,
                      uint64_t payload_size,
                      std::wstring* error) {
  if (input == nullptr || !input->is_open()) {
    if (error != nullptr) {
      *error = L"MP4 payload dry run needs an open packet file.";
    }
    return false;
  }

  if (payload_offset >
      static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    if (error != nullptr) {
      *error = L"MP4 payload dry run packet offset is too large.";
    }
    return false;
  }

  input->clear();
  input->seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  if (!input->good()) {
    if (error != nullptr) {
      *error = L"MP4 payload dry run could not seek to a packet payload.";
    }
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  uint64_t remaining = payload_size;
  while (remaining > 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<uint64_t>(remaining, buffer.size()));
    input->read(buffer.data(), chunk);
    if (input->gcount() != chunk) {
      if (error != nullptr) {
        *error = L"MP4 payload dry run could not read a full packet payload.";
      }
      return false;
    }
    remaining -= static_cast<uint64_t>(chunk);
  }

  return true;
}

bool ReadPacketPayload(std::ifstream* input,
                       const PacketIndexEntry& entry,
                       std::vector<uint8_t>* payload,
                       std::wstring* error) {
  if (input == nullptr || !input->is_open() || payload == nullptr) {
    if (error != nullptr) {
      *error = L"MP4 mux packet read needs an open packet file and output "
               L"buffer.";
    }
    return false;
  }

  if (entry.packet_size < entry.payload_size) {
    if (error != nullptr) {
      *error = L"MP4 mux packet size is invalid.";
    }
    return false;
  }

  const uint64_t header_size = entry.packet_size - entry.payload_size;
  uint64_t payload_offset = 0;
  if (!AddU64(entry.file_offset, header_size, &payload_offset)) {
    if (error != nullptr) {
      *error = L"MP4 mux packet payload offset overflowed.";
    }
    return false;
  }

  if (payload_offset >
          static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      entry.payload_size >
          static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
      entry.payload_size >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    if (error != nullptr) {
      *error = L"MP4 mux packet payload is too large.";
    }
    return false;
  }

  payload->assign(static_cast<size_t>(entry.payload_size), 0);
  input->clear();
  input->seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  if (!input->good()) {
    if (error != nullptr) {
      *error = L"MP4 mux could not seek to a packet payload.";
    }
    return false;
  }

  if (!payload->empty()) {
    input->read(reinterpret_cast<char*>(payload->data()),
                static_cast<std::streamsize>(payload->size()));
    if (input->gcount() !=
        static_cast<std::streamsize>(payload->size())) {
      if (error != nullptr) {
        *error = L"MP4 mux could not read a full packet payload.";
      }
      return false;
    }
  }

  return true;
}

bool ValidatePacketWriterRequest(const Mp4MuxRequest& request,
                                 std::wstring* error) {
  std::map<uint32_t, int64_t> previous_dts_by_track;
  for (const auto& packet : request.plan.packets) {
    const auto& metadata = packet.packet.metadata;
    if (packet.output_pts_ns < 0 || packet.output_dts_ns < 0 ||
        packet.duration_ns <= 0 || packet.output_pts_ns < packet.output_dts_ns ||
        packet.packet.payload_size == 0) {
      if (error != nullptr) {
        *error = L"MP4 packet writing needs non-negative timestamps, "
                 L"positive durations, and nonempty payloads.";
      }
      return false;
    }

    const auto previous = previous_dts_by_track.find(metadata.track_id);
    if (previous != previous_dts_by_track.end() &&
        packet.output_dts_ns < previous->second) {
      if (error != nullptr) {
        *error = L"MP4 packet writing DTS is not monotonic within a track.";
      }
      return false;
    }
    previous_dts_by_track[metadata.track_id] = packet.output_dts_ns;
  }

  return true;
}

}  // namespace

bool Mp4H264VideoTrack::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::H264 && width > 0 &&
         height > 0 && fps_numerator > 0 && fps_denominator > 0 &&
         packet_format == L"annex_b" && !sps.empty() && !pps.empty() &&
         !avcc_extradata.empty();
}

bool Mp4AacAudioTrack::IsReady() const noexcept {
  return track_id != 0 && codec_id == CodecId::Aac && !source_kind.empty() &&
         !name.empty() && (sample_rate == 44100 || sample_rate == 48000) &&
         (channel_count == 1 || channel_count == 2 || channel_count == 6) &&
         bitrate_bps > 0 && frame_samples == 1024 && payload_type == 0 &&
         audio_object_type == 2 && !audio_specific_config.empty() &&
         !encoder_name.empty();
}

bool Mp4MuxBackendAvailability::Available() const noexcept {
  return status == Mp4MuxBackendStatus::Configured;
}

bool Mp4MuxResult::Succeeded() const noexcept {
  return status == Mp4MuxStatus::Success;
}

bool Mp4MuxPayloadReadStats::HasVideoPackets() const noexcept {
  return video_packet_count > 0;
}

bool Mp4MuxStreamSetupStats::IsReady() const noexcept {
  return output_context_allocated && video_stream_created &&
         h264_parameters_applied && video_track_id != 0 && width > 0 &&
         height > 0 && fps_numerator > 0 && fps_denominator > 0 &&
         extradata_bytes > 0 &&
         audio_stream_count == aac_parameters_applied_count &&
         (audio_stream_count == 0 || audio_extradata_bytes > 0);
}

Mp4MuxResult BuildVideoMp4MuxRequest(
    const VideoExportPlan& export_plan,
    const std::filesystem::path& temp_output_path,
    const std::filesystem::path& final_output_path,
    bool allow_overwrite,
    Mp4MuxRequest* request) {
  if (request == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request builder needs an output destination.");
  }

  *request = {};
  if (!export_plan.IsReady()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request builder needs a ready video export plan.");
  }

  request->temp_output_path = temp_output_path;
  request->final_output_path = final_output_path;
  request->packet_file_path = export_plan.packet_file_path;
  request->video_track = BuildMp4VideoTrack(export_plan.video);
  request->audio_tracks.reserve(export_plan.audio_tracks.size());
  for (const auto& audio : export_plan.audio_tracks) {
    request->audio_tracks.push_back(BuildMp4AudioTrack(audio));
  }
  request->plan = export_plan.mux_plan;
  request->allow_overwrite = allow_overwrite;
  return Mp4Muxer::ValidateRequest(*request);
}

const wchar_t* Mp4MuxStatusName(Mp4MuxStatus status) noexcept {
  switch (status) {
    case Mp4MuxStatus::Success:
      return L"success";
    case Mp4MuxStatus::InvalidRequest:
      return L"invalid request";
    case Mp4MuxStatus::BackendUnavailable:
      return L"backend unavailable";
    case Mp4MuxStatus::DestinationExists:
      return L"destination exists";
    case Mp4MuxStatus::FileSystemError:
      return L"file system error";
  }

  return L"unknown";
}

const wchar_t* Mp4MuxBackendStatusName(
    Mp4MuxBackendStatus status) noexcept {
  switch (status) {
    case Mp4MuxBackendStatus::NotConfigured:
      return L"not configured";
    case Mp4MuxBackendStatus::Configured:
      return L"configured";
  }

  return L"unknown";
}

Mp4MuxResult Mp4Muxer::WriteMp4(const Mp4MuxRequest& request) const {
  const auto validation = ValidateRequest(request);
  if (!validation.Succeeded()) {
    return validation;
  }

  const auto availability = BackendAvailability();
  if (!availability.Available()) {
    return Result(Mp4MuxStatus::BackendUnavailable, availability.message);
  }

#if OLOUIE_FFMPEG_CONFIGURED
  return WriteFfmpegMp4(request);
#else
  return Result(Mp4MuxStatus::BackendUnavailable,
                L"FFmpeg MP4 mux backend is configured, but packet writing is "
                L"intentionally deferred.");
#endif
}

Mp4MuxBackendAvailability Mp4Muxer::BackendAvailability() {
  Mp4MuxBackendAvailability availability;
  availability.dynamic_linking_expected = true;
  availability.required_libraries = {
      L"avformat", L"avcodec", L"avutil", L"swresample"};
#if OLOUIE_FFMPEG_CONFIGURED
  availability.status = Mp4MuxBackendStatus::Configured;
  availability.message =
      L"LGPL dynamic FFmpeg headers and import libraries were configured by "
      L"CMake.";
#else
  availability.status = Mp4MuxBackendStatus::NotConfigured;
  availability.message =
      L"FFmpeg MP4 mux backend is not configured. Configure with "
      L"OLOUIE_ENABLE_FFMPEG=ON and an LGPL dynamic FFmpeg root.";
#endif
  return availability;
}

Mp4MuxResult Mp4Muxer::DryRunPayloadRead(
    const Mp4MuxRequest& request,
    Mp4MuxPayloadReadStats* stats) {
  if (stats == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 payload dry run needs a stats destination.");
  }

  *stats = {};
  const auto validation = ValidateRequest(request);
  if (!validation.Succeeded()) {
    return validation;
  }

  std::ifstream input(request.packet_file_path, std::ios::binary);
  if (!input.is_open()) {
    return Result(Mp4MuxStatus::FileSystemError,
                  L"MP4 payload dry run could not open packets.dat.");
  }

  bool have_previous_video_dts = false;
  int64_t previous_video_dts_ns = 0;
  std::map<uint32_t, int64_t> previous_dts_by_track;

  for (const auto& packet : request.plan.packets) {
    const auto& entry = packet.packet;
    if (packet.output_pts_ns < 0 || packet.output_dts_ns < 0 ||
        packet.duration_ns <= 0 ||
        packet.output_pts_ns < packet.output_dts_ns) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run packet timestamps are invalid.");
    }
    const auto previous =
        previous_dts_by_track.find(entry.metadata.track_id);
    if (previous != previous_dts_by_track.end() &&
        packet.output_dts_ns < previous->second) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run track DTS is not monotonic.");
    }
    previous_dts_by_track[entry.metadata.track_id] = packet.output_dts_ns;

    if (entry.packet_size < entry.payload_size) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run packet size is invalid.");
    }

    const uint64_t header_size = entry.packet_size - entry.payload_size;
    uint64_t payload_offset = 0;
    if (!AddU64(entry.file_offset, header_size, &payload_offset)) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run packet offset overflowed.");
    }

    if (entry.payload_size >
        static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run packet payload is too large.");
    }

    std::wstring read_error;
    if (!ReadPayloadBytes(&input, payload_offset, entry.payload_size,
                          &read_error)) {
      return Result(Mp4MuxStatus::FileSystemError, std::move(read_error));
    }

    if (!AddPayloadBytes(entry.payload_size, &stats->payload_byte_count)) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 payload dry run payload byte count overflowed.");
    }
    ++stats->packet_count;

    if (entry.metadata.track_id == request.video_track.track_id &&
        entry.metadata.codec_id == CodecId::H264) {
      if (have_previous_video_dts &&
          packet.output_dts_ns < previous_video_dts_ns) {
        return Result(Mp4MuxStatus::InvalidRequest,
                      L"MP4 payload dry run video DTS is not monotonic.");
      }

      if (!have_previous_video_dts) {
        stats->first_video_dts_ns = packet.output_dts_ns;
      }
      have_previous_video_dts = true;
      previous_video_dts_ns = packet.output_dts_ns;
      stats->last_video_dts_ns = packet.output_dts_ns;

      if (!AddPayloadBytes(entry.payload_size,
                           &stats->video_payload_byte_count)) {
        return Result(
            Mp4MuxStatus::InvalidRequest,
            L"MP4 payload dry run video payload byte count overflowed.");
      }
      ++stats->video_packet_count;
    } else if (entry.metadata.codec_id == CodecId::Aac) {
      if (!AddPayloadBytes(entry.payload_size,
                           &stats->audio_payload_byte_count)) {
        return Result(
            Mp4MuxStatus::InvalidRequest,
            L"MP4 payload dry run audio payload byte count overflowed.");
      }
      ++stats->audio_packet_count;
    }
  }

  if (!stats->HasVideoPackets()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 payload dry run did not see video packets.");
  }

  return Result(Mp4MuxStatus::Success,
                L"MP4 payload dry run read all referenced packet payloads.");
}

Mp4MuxResult Mp4Muxer::ValidateVideoStreamSetup(
    const Mp4MuxRequest& request,
    Mp4MuxStreamSetupStats* stats) {
  return ValidateStreamSetup(request, stats);
}

Mp4MuxResult Mp4Muxer::ValidateStreamSetup(
    const Mp4MuxRequest& request,
    Mp4MuxStreamSetupStats* stats) {
  if (stats == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 stream setup validation needs a stats destination.");
  }

  *stats = {};
  const auto validation = ValidateRequest(request);
  if (!validation.Succeeded()) {
    return validation;
  }

  const auto availability = BackendAvailability();
  if (!availability.Available()) {
    return Result(Mp4MuxStatus::BackendUnavailable, availability.message);
  }

#if OLOUIE_FFMPEG_CONFIGURED
  AVFormatContext* context = nullptr;
  const int context_result =
      avformat_alloc_output_context2(&context, nullptr, "mp4", nullptr);
  if (context_result < 0 || context == nullptr) {
    return Result(Mp4MuxStatus::BackendUnavailable,
                  L"FFmpeg could not allocate an MP4 output context.");
  }

  std::map<uint32_t, AVStream*> streams;
  const auto stream_setup =
      ConfigureFfmpegStreams(context, request, &streams, stats);
  avformat_free_context(context);
  return stream_setup;
#else
  return Result(Mp4MuxStatus::BackendUnavailable, availability.message);
#endif
}

Mp4MuxResult Mp4Muxer::ValidateRequest(const Mp4MuxRequest& request) {
  if (request.temp_output_path.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request needs a temporary output path.");
  }

  if (request.final_output_path.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request needs a final output path.");
  }

  if (request.packet_file_path.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request needs a packet file path.");
  }

  if (!IsMp4Path(request.final_output_path)) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux final output path must use the .mp4 extension.");
  }

  if (request.temp_output_path == request.final_output_path) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux temporary and final paths must be different.");
  }

  if (request.plan.source_end_ns <= request.plan.source_start_ns) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux plan must have a positive source duration.");
  }

  if (request.plan.tracks.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux plan needs at least one track.");
  }

  if (request.plan.packets.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux plan needs at least one packet.");
  }

  if (!request.plan.HasVideoTrack()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux plan needs a video track for MVP exports.");
  }

  if (!request.video_track.IsReady()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request needs complete H.264 video metadata.");
  }

  const auto* video_track = FindSingleVideoTrack(request.plan);
  if (video_track == nullptr) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux request needs exactly one H.264 video track.");
  }

  if (video_track->track_id != request.video_track.track_id ||
      video_track->codec_id != request.video_track.codec_id) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux video metadata must match the mux plan track.");
  }

  std::map<uint32_t, CodecId> plan_tracks;
  for (const auto& track : request.plan.tracks) {
    if (track.track_id == 0 ||
        (track.codec_id != CodecId::H264 &&
         track.codec_id != CodecId::Aac) ||
        !plan_tracks.emplace(track.track_id, track.codec_id).second) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 mux plan contains invalid or duplicate tracks.");
    }
  }

  std::set<uint32_t> audio_track_ids;
  for (const auto& audio : request.audio_tracks) {
    if (!audio.IsReady() || audio.track_id == request.video_track.track_id ||
        !audio_track_ids.insert(audio.track_id).second) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 mux request contains invalid or duplicate AAC "
                    L"metadata.");
    }
    const auto plan_track = plan_tracks.find(audio.track_id);
    if (plan_track == plan_tracks.end() ||
        plan_track->second != CodecId::Aac) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 mux AAC metadata must match a mux plan track.");
    }
  }

  for (const auto& track : request.plan.tracks) {
    if (track.codec_id == CodecId::Aac &&
        !audio_track_ids.contains(track.track_id)) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 mux plan AAC tracks need complete stream "
                    L"metadata.");
    }
  }

  std::set<uint32_t> packet_track_ids;
  for (const auto& packet : request.plan.packets) {
    const auto track =
        plan_tracks.find(packet.packet.metadata.track_id);
    if (track == plan_tracks.end() ||
        track->second != packet.packet.metadata.codec_id) {
      return Result(Mp4MuxStatus::InvalidRequest,
                    L"MP4 mux packet metadata does not match its plan "
                    L"track.");
    }
    packet_track_ids.insert(packet.packet.metadata.track_id);
  }
  if (packet_track_ids.size() != plan_tracks.size()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux plan contains a stream without packets.");
  }

  const auto* first_video_packet =
      FindFirstVideoPacket(request.plan, request.video_track.track_id);
  if (first_video_packet == nullptr || !first_video_packet->packet.IsKeyframe()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"MP4 mux video packets must start on a keyframe.");
  }

  return Result(Mp4MuxStatus::Success, L"MP4 mux request is valid.");
}

Mp4MuxResult Mp4Muxer::AtomicRename(const std::filesystem::path& temp_path,
                                    const std::filesystem::path& final_path,
                                    bool allow_overwrite) {
  if (temp_path.empty() || final_path.empty()) {
    return Result(Mp4MuxStatus::InvalidRequest,
                  L"Atomic rename needs both temporary and final paths.");
  }

  std::error_code error;
  const auto parent = final_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      auto fault = MakeDiskWriteFault(
          DiskWriteSubsystem::Mp4Mux,
          DiskWriteOperation::CreateDirectories, parent, error);
      return Result(Mp4MuxStatus::FileSystemError,
                    DescribeDiskWriteFault(fault), fault);
    }
  }

  DiskWriteFault fault;
  if (!AtomicPublishFile(temp_path, final_path, allow_overwrite,
                         DiskWriteSubsystem::Mp4Mux, &fault)) {
    const auto status =
        fault.kind == DiskWriteFaultKind::DestinationExists
            ? Mp4MuxStatus::DestinationExists
            : Mp4MuxStatus::FileSystemError;
    return Result(status, DescribeDiskWriteFault(fault), fault);
  }

  return Result(Mp4MuxStatus::Success, L"Atomic rename completed.");
}

}  // namespace olouie::record
