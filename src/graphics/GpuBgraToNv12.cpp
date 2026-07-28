#include "graphics/GpuBgraToNv12.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace olouie::graphics {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

std::wstring HResultToHex(HRESULT result) {
  wchar_t buffer[12]{};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

GpuBgraToNv12SmokeResult Result(GpuBgraToNv12Status status,
                                std::wstring message) {
  GpuBgraToNv12SmokeResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

GpuBgraToNv12ConvertResult ConversionResult(
    GpuBgraToNv12Status status,
    std::wstring message,
    HRESULT hresult = S_OK,
    ID3D11Device* device = nullptr,
    std::wstring operation = {}) {
  GpuBgraToNv12ConvertResult result;
  result.status = status;
  result.message = std::move(message);
  result.hresult = hresult;
  result.device_fault = InspectD3D11DeviceFault(
      device, hresult, std::move(operation));
  if (result.device_fault.Failed()) {
    result.message = result.device_fault.message;
  }
  return result;
}

}  // namespace

bool GpuBgraToNv12Plan::IsValid() const noexcept {
  return source_width != 0 && source_height != 0 && output_width != 0 &&
         output_height != 0 && (output_width % 2u) == 0 &&
         (output_height % 2u) == 0 &&
         source_format == DXGI_FORMAT_B8G8R8A8_UNORM &&
         output_format == DXGI_FORMAT_NV12;
}

bool GpuBgraToNv12SmokeResult::Succeeded() const noexcept {
  return status == GpuBgraToNv12Status::Success;
}

bool GpuBgraToNv12ConvertResult::Succeeded() const noexcept {
  return status == GpuBgraToNv12Status::Success;
}

bool GpuBgraToNv12Converter::IsInitialized() const noexcept {
  return plan_.IsValid() && device_ != nullptr && video_device_ != nullptr &&
         video_context_ != nullptr && enumerator_ != nullptr &&
         processor_ != nullptr;
}

GpuBgraToNv12ConvertResult GpuBgraToNv12Converter::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const GpuBgraToNv12Config& config) {
  *this = GpuBgraToNv12Converter();

  if (device == nullptr || context == nullptr) {
    return ConversionResult(
        GpuBgraToNv12Status::InvalidConfig,
        L"BGRA-to-NV12 converter needs a D3D11 device and context.");
  }

  GpuBgraToNv12Plan plan;
  std::wstring error;
  if (!BuildGpuBgraToNv12Plan(config, &plan, &error)) {
    return ConversionResult(GpuBgraToNv12Status::InvalidConfig,
                            std::move(error));
  }

  winrt::com_ptr<ID3D11VideoDevice> video_device;
  HRESULT hr =
      device->QueryInterface(__uuidof(ID3D11VideoDevice),
                             video_device.put_void());
  if (FAILED(hr)) {
    return ConversionResult(
        GpuBgraToNv12Status::VideoDeviceUnavailable,
        L"D3D11 device does not expose ID3D11VideoDevice (" +
            HResultToHex(hr) + L").",
        hr, device, L"video-device query");
  }

  winrt::com_ptr<ID3D11VideoContext> video_context;
  hr = context->QueryInterface(__uuidof(ID3D11VideoContext),
                               video_context.put_void());
  if (FAILED(hr)) {
    return ConversionResult(
        GpuBgraToNv12Status::VideoDeviceUnavailable,
        L"D3D11 context does not expose ID3D11VideoContext (" +
            HResultToHex(hr) + L").",
        hr, device, L"video-context query");
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
  content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content_desc.InputWidth = plan.source_width;
  content_desc.InputHeight = plan.source_height;
  content_desc.OutputWidth = plan.output_width;
  content_desc.OutputHeight = plan.output_height;
  content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
  hr = video_device->CreateVideoProcessorEnumerator(&content_desc,
                                                    enumerator.put());
  if (FAILED(hr)) {
    return ConversionResult(
        GpuBgraToNv12Status::ConversionUnsupported,
        L"Could not create D3D11 VideoProcessor enumerator (" +
            HResultToHex(hr) + L").",
        hr, device, L"VideoProcessor enumerator creation");
  }

  UINT flags = 0;
  hr = enumerator->CheckVideoProcessorFormat(plan.source_format, &flags);
  if (FAILED(hr) ||
      (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    return ConversionResult(
        GpuBgraToNv12Status::ConversionUnsupported,
        L"D3D11 VideoProcessor does not support BGRA input (" +
            HResultToHex(hr) + L").",
        hr, device, L"VideoProcessor BGRA format check");
  }

  flags = 0;
  hr = enumerator->CheckVideoProcessorFormat(plan.output_format, &flags);
  if (FAILED(hr) ||
      (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    return ConversionResult(
        GpuBgraToNv12Status::ConversionUnsupported,
        L"D3D11 VideoProcessor does not support NV12 output (" +
            HResultToHex(hr) + L").",
        hr, device, L"VideoProcessor NV12 format check");
  }

  winrt::com_ptr<ID3D11VideoProcessor> processor;
  hr = video_device->CreateVideoProcessor(enumerator.get(), 0,
                                          processor.put());
  if (FAILED(hr)) {
    return ConversionResult(
        GpuBgraToNv12Status::ProcessorCreateFailed,
        L"Could not create D3D11 VideoProcessor (" + HResultToHex(hr) +
            L").",
        hr, device, L"VideoProcessor creation");
  }

  plan_ = plan;
  device_.copy_from(device);
  video_device_ = std::move(video_device);
  video_context_ = std::move(video_context);
  enumerator_ = std::move(enumerator);
  processor_ = std::move(processor);

  return ConversionResult(GpuBgraToNv12Status::Success, L"");
}

GpuBgraToNv12ConvertResult GpuBgraToNv12Converter::Convert(
    ID3D11Texture2D* source_bgra,
    ID3D11Texture2D* output_nv12) {
  if (!IsInitialized()) {
    return ConversionResult(GpuBgraToNv12Status::InvalidConfig,
                            L"BGRA-to-NV12 converter is not initialized.");
  }
  if (source_bgra == nullptr || output_nv12 == nullptr) {
    return ConversionResult(
        GpuBgraToNv12Status::InvalidConfig,
        L"BGRA-to-NV12 conversion needs source and output textures.");
  }

  D3D11_TEXTURE2D_DESC source_desc{};
  source_bgra->GetDesc(&source_desc);
  if (source_desc.Width != plan_.source_width ||
      source_desc.Height != plan_.source_height ||
      source_desc.Format != plan_.source_format ||
      source_desc.SampleDesc.Count != 1) {
    return ConversionResult(
        GpuBgraToNv12Status::InvalidConfig,
        L"BGRA source texture does not match the initialized conversion "
        L"plan.");
  }

  D3D11_TEXTURE2D_DESC output_desc{};
  output_nv12->GetDesc(&output_desc);
  if (output_desc.Width != plan_.output_width ||
      output_desc.Height != plan_.output_height ||
      output_desc.Format != plan_.output_format ||
      output_desc.SampleDesc.Count != 1) {
    return ConversionResult(
        GpuBgraToNv12Status::InvalidConfig,
        L"NV12 output texture does not match the initialized conversion "
        L"plan.");
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc{};
  input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_view_desc.Texture2D.MipSlice = 0;
  input_view_desc.Texture2D.ArraySlice = 0;

  ID3D11VideoProcessorInputView* input_view = nullptr;
  for (auto& cached : input_views_) {
    if (cached.texture.get() == source_bgra) {
      input_view = cached.view.get();
      ++stats_.input_view_reuse_count;
      break;
    }
  }

  HRESULT hr = S_OK;
  if (input_view == nullptr) {
    CachedInputView cached;
    cached.texture.copy_from(source_bgra);
    hr = video_device_->CreateVideoProcessorInputView(
        source_bgra, enumerator_.get(), &input_view_desc, cached.view.put());
    if (FAILED(hr)) {
      return ConversionResult(
          GpuBgraToNv12Status::ViewCreateFailed,
          L"Could not create D3D11 VideoProcessor input view (" +
              HResultToHex(hr) + L").",
          hr, device_.get(), L"VideoProcessor input-view creation");
    }

    ++stats_.input_view_create_count;
    input_view = cached.view.get();
    if (input_views_.size() < kMaximumCachedInputViewCount) {
      input_views_.push_back(std::move(cached));
    } else {
      input_views_[next_input_view_eviction_index_] = std::move(cached);
      next_input_view_eviction_index_ =
          (next_input_view_eviction_index_ + 1) %
          kMaximumCachedInputViewCount;
      ++stats_.input_view_eviction_count;
    }
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
  output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_view_desc.Texture2D.MipSlice = 0;

  if (output_view_texture_.get() == output_nv12 && output_view_ != nullptr) {
    ++stats_.output_view_reuse_count;
  } else {
    winrt::com_ptr<ID3D11VideoProcessorOutputView> output_view;
    hr = video_device_->CreateVideoProcessorOutputView(
        output_nv12, enumerator_.get(), &output_view_desc,
        output_view.put());
    if (FAILED(hr)) {
      return ConversionResult(
          GpuBgraToNv12Status::ViewCreateFailed,
          L"Could not create D3D11 VideoProcessor output view (" +
              HResultToHex(hr) + L").",
          hr, device_.get(), L"VideoProcessor output-view creation");
    }
    output_view_texture_.copy_from(output_nv12);
    output_view_ = std::move(output_view);
    ++stats_.output_view_create_count;
  }

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view;

  const auto conversion_started = std::chrono::steady_clock::now();
  hr = video_context_->VideoProcessorBlt(processor_.get(), output_view_.get(),
                                         0, 1, &stream);
  const auto conversion_finished = std::chrono::steady_clock::now();
  const auto conversion_latency_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          conversion_finished - conversion_started)
          .count());
  ++stats_.conversion_submission_count;
  stats_.last_conversion_submission_latency_ns = conversion_latency_ns;
  stats_.maximum_conversion_submission_latency_ns = std::max(
      stats_.maximum_conversion_submission_latency_ns,
      conversion_latency_ns);
  stats_.total_conversion_submission_latency_ns += conversion_latency_ns;
  if (FAILED(hr)) {
    return ConversionResult(
        GpuBgraToNv12Status::ConversionFailed,
        L"D3D11 VideoProcessor BGRA-to-NV12 blit failed (" +
            HResultToHex(hr) + L").",
        hr, device_.get(), L"BGRA-to-NV12 VideoProcessor blit");
  }

  auto completed = ConversionResult(
      GpuBgraToNv12Status::Success, L"", S_OK, device_.get(),
      L"BGRA-to-NV12 VideoProcessor blit");
  if (completed.device_fault.Failed()) {
    completed.status = GpuBgraToNv12Status::ConversionFailed;
  } else {
    ++stats_.converted_frame_count;
  }
  return completed;
}

bool BuildGpuBgraToNv12Plan(const GpuBgraToNv12Config& config,
                            GpuBgraToNv12Plan* plan,
                            std::wstring* error) {
  if (plan == nullptr) {
    SetError(error, L"BGRA-to-NV12 plan needs an output destination.");
    return false;
  }

  *plan = {};
  if (config.source_width == 0 || config.source_height == 0 ||
      config.output_width == 0 || config.output_height == 0) {
    SetError(error, L"BGRA-to-NV12 dimensions must be nonzero.");
    return false;
  }
  if ((config.output_width % 2u) != 0 || (config.output_height % 2u) != 0) {
    SetError(error, L"NV12 output dimensions must be even.");
    return false;
  }

  plan->source_width = config.source_width;
  plan->source_height = config.source_height;
  plan->output_width = config.output_width;
  plan->output_height = config.output_height;
  plan->source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  plan->output_format = DXGI_FORMAT_NV12;
  return true;
}

GpuBgraToNv12SmokeResult CreateGpuBgraToNv12SmokeTextures(
    ID3D11Device* device,
    const GpuBgraToNv12Config& config) {
  if (device == nullptr) {
    return Result(GpuBgraToNv12Status::InvalidConfig,
                  L"BGRA-to-NV12 smoke needs a D3D11 device.");
  }

  GpuBgraToNv12Plan plan;
  std::wstring error;
  if (!BuildGpuBgraToNv12Plan(config, &plan, &error)) {
    return Result(GpuBgraToNv12Status::InvalidConfig, std::move(error));
  }

  D3D11_TEXTURE2D_DESC source_desc{};
  source_desc.Width = plan.source_width;
  source_desc.Height = plan.source_height;
  source_desc.MipLevels = 1;
  source_desc.ArraySize = 1;
  source_desc.Format = plan.source_format;
  source_desc.SampleDesc.Count = 1;
  source_desc.Usage = D3D11_USAGE_DEFAULT;
  source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  winrt::com_ptr<ID3D11Texture2D> source_texture;
  HRESULT hr = device->CreateTexture2D(&source_desc, nullptr,
                                       source_texture.put());
  if (FAILED(hr)) {
    auto result =
        Result(GpuBgraToNv12Status::TextureCreateFailed,
               L"Could not create app-owned BGRA source texture (" +
                   HResultToHex(hr) + L").");
    result.plan = plan;
    return result;
  }

  D3D11_TEXTURE2D_DESC output_desc{};
  output_desc.Width = plan.output_width;
  output_desc.Height = plan.output_height;
  output_desc.MipLevels = 1;
  output_desc.ArraySize = 1;
  output_desc.Format = plan.output_format;
  output_desc.SampleDesc.Count = 1;
  output_desc.Usage = D3D11_USAGE_DEFAULT;

  winrt::com_ptr<ID3D11Texture2D> output_texture;
  hr = device->CreateTexture2D(&output_desc, nullptr, output_texture.put());
  if (FAILED(hr)) {
    auto result =
        Result(GpuBgraToNv12Status::TextureCreateFailed,
               L"Could not create app-owned NV12 output texture (" +
                   HResultToHex(hr) + L").");
    result.plan = plan;
    result.source_texture_created = true;
    return result;
  }

  auto result = Result(GpuBgraToNv12Status::Success, L"");
  result.plan = plan;
  result.source_texture_created = true;
  result.output_texture_created = true;
  return result;
}

GpuBgraToNv12SmokeResult RunGpuBgraToNv12VideoProcessorSmoke(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const GpuBgraToNv12Config& config) {
  if (device == nullptr || context == nullptr) {
    return Result(GpuBgraToNv12Status::InvalidConfig,
                  L"BGRA-to-NV12 VideoProcessor smoke needs a D3D11 device "
                  L"and context.");
  }

  GpuBgraToNv12Plan plan;
  std::wstring error;
  if (!BuildGpuBgraToNv12Plan(config, &plan, &error)) {
    return Result(GpuBgraToNv12Status::InvalidConfig, std::move(error));
  }

  GpuBgraToNv12Converter converter;
  const auto init_result = converter.Initialize(device, context, config);
  if (!init_result.Succeeded()) {
    auto result = Result(init_result.status, init_result.message);
    result.plan = plan;
    return result;
  }

  std::vector<uint8_t> bgra(static_cast<size_t>(plan.source_width) *
                            plan.source_height * 4u);
  for (uint32_t y = 0; y < plan.source_height; ++y) {
    for (uint32_t x = 0; x < plan.source_width; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * plan.source_width + x) * 4u;
      bgra[offset + 0u] = static_cast<uint8_t>(x % 256u);
      bgra[offset + 1u] = static_cast<uint8_t>(y % 256u);
      bgra[offset + 2u] = static_cast<uint8_t>((x + y) % 256u);
      bgra[offset + 3u] = 255u;
    }
  }

  D3D11_TEXTURE2D_DESC source_desc{};
  source_desc.Width = plan.source_width;
  source_desc.Height = plan.source_height;
  source_desc.MipLevels = 1;
  source_desc.ArraySize = 1;
  source_desc.Format = plan.source_format;
  source_desc.SampleDesc.Count = 1;
  source_desc.Usage = D3D11_USAGE_DEFAULT;

  D3D11_SUBRESOURCE_DATA source_data{};
  source_data.pSysMem = bgra.data();
  source_data.SysMemPitch = plan.source_width * 4u;

  D3D11_TEXTURE2D_DESC output_desc{};
  output_desc.Width = plan.output_width;
  output_desc.Height = plan.output_height;
  output_desc.MipLevels = 1;
  output_desc.ArraySize = 1;
  output_desc.Format = plan.output_format;
  output_desc.SampleDesc.Count = 1;
  output_desc.Usage = D3D11_USAGE_DEFAULT;
  output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  winrt::com_ptr<ID3D11Texture2D> output_texture;
  HRESULT hr = device->CreateTexture2D(&output_desc, nullptr,
                                       output_texture.put());
  if (FAILED(hr)) {
    auto result =
        Result(GpuBgraToNv12Status::TextureCreateFailed,
               L"Could not create NV12 VideoProcessor output texture (" +
                   HResultToHex(hr) + L").");
    result.plan = plan;
    return result;
  }

  winrt::com_ptr<ID3D11Texture2D> source_texture;
  GpuBgraToNv12ConvertResult last_convert_result{
      GpuBgraToNv12Status::InvalidConfig, L""};
  HRESULT last_source_hr = S_OK;
  const UINT source_bind_candidates[] = {
      D3D11_BIND_SHADER_RESOURCE,
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
      0u,
      D3D11_BIND_DECODER,
  };
  for (const UINT bind_flags : source_bind_candidates) {
    source_desc.BindFlags = bind_flags;
    winrt::com_ptr<ID3D11Texture2D> candidate_texture;
    last_source_hr = device->CreateTexture2D(&source_desc, &source_data,
                                             candidate_texture.put());
    if (FAILED(last_source_hr)) {
      continue;
    }

    last_convert_result =
        converter.Convert(candidate_texture.get(), output_texture.get());
    if (last_convert_result.Succeeded()) {
      source_texture = std::move(candidate_texture);
      break;
    }
  }

  if (source_texture == nullptr) {
    auto result =
        Result(last_convert_result.status,
               L"Could not execute BGRA-to-NV12 conversion with any probed "
               L"source texture binding (last texture " +
                   HResultToHex(last_source_hr) + L", last convert: " +
                   last_convert_result.message + L").");
    result.plan = plan;
    result.output_texture_created = true;
    result.video_processor_created = true;
    return result;
  }

  const auto reuse_convert =
      converter.Convert(source_texture.get(), output_texture.get());
  if (!reuse_convert.Succeeded()) {
    auto result = Result(
        reuse_convert.status,
        L"Could not reuse VideoProcessor views for a repeated conversion: " +
            reuse_convert.message);
    result.plan = plan;
    result.source_texture_created = true;
    result.output_texture_created = true;
    result.video_processor_created = true;
    return result;
  }

  auto result = Result(GpuBgraToNv12Status::Success, L"");
  result.plan = plan;
  result.source_texture_created = true;
  result.output_texture_created = true;
  result.video_processor_created = true;
  result.conversion_executed = true;
  result.input_view_reused =
      converter.stats().input_view_reuse_count != 0;
  result.output_view_reused =
      converter.stats().output_view_reuse_count != 0;
  return result;
}

const wchar_t* GpuBgraToNv12StatusName(
    GpuBgraToNv12Status status) noexcept {
  switch (status) {
    case GpuBgraToNv12Status::Success:
      return L"success";
    case GpuBgraToNv12Status::InvalidConfig:
      return L"invalid config";
    case GpuBgraToNv12Status::TextureCreateFailed:
      return L"texture create failed";
    case GpuBgraToNv12Status::VideoDeviceUnavailable:
      return L"video device unavailable";
    case GpuBgraToNv12Status::ConversionUnsupported:
      return L"conversion unsupported";
    case GpuBgraToNv12Status::ProcessorCreateFailed:
      return L"processor create failed";
    case GpuBgraToNv12Status::ViewCreateFailed:
      return L"view create failed";
    case GpuBgraToNv12Status::ConversionFailed:
      return L"conversion failed";
  }

  return L"unknown";
}

}  // namespace olouie::graphics
