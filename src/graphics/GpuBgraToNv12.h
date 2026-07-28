#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgiformat.h>
#include <winrt/base.h>

#include "graphics/D3D11DeviceFault.h"

namespace olouie::graphics {

enum class GpuBgraToNv12Status {
  Success,
  InvalidConfig,
  TextureCreateFailed,
  VideoDeviceUnavailable,
  ConversionUnsupported,
  ProcessorCreateFailed,
  ViewCreateFailed,
  ConversionFailed,
};

struct GpuBgraToNv12Config {
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
};

struct GpuBgraToNv12Plan {
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  DXGI_FORMAT source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  DXGI_FORMAT output_format = DXGI_FORMAT_NV12;

  bool IsValid() const noexcept;
};

struct GpuBgraToNv12SmokeResult {
  GpuBgraToNv12Status status = GpuBgraToNv12Status::InvalidConfig;
  std::wstring message;
  GpuBgraToNv12Plan plan;
  bool source_texture_created = false;
  bool output_texture_created = false;
  bool video_processor_created = false;
  bool conversion_executed = false;
  bool input_view_reused = false;
  bool output_view_reused = false;

  bool Succeeded() const noexcept;
};

struct GpuBgraToNv12ConvertResult {
  GpuBgraToNv12Status status = GpuBgraToNv12Status::InvalidConfig;
  std::wstring message;
  HRESULT hresult = S_OK;
  D3D11DeviceFault device_fault;

  bool Succeeded() const noexcept;
};

struct GpuBgraToNv12ConverterStats {
  uint64_t converted_frame_count = 0;
  uint64_t input_view_create_count = 0;
  uint64_t input_view_reuse_count = 0;
  uint64_t input_view_eviction_count = 0;
  uint64_t output_view_create_count = 0;
  uint64_t output_view_reuse_count = 0;
  uint64_t conversion_submission_count = 0;
  uint64_t last_conversion_submission_latency_ns = 0;
  uint64_t maximum_conversion_submission_latency_ns = 0;
  uint64_t total_conversion_submission_latency_ns = 0;
};

class GpuBgraToNv12Converter {
 public:
  GpuBgraToNv12Converter() = default;
  GpuBgraToNv12Converter(const GpuBgraToNv12Converter&) = delete;
  GpuBgraToNv12Converter& operator=(const GpuBgraToNv12Converter&) = delete;
  GpuBgraToNv12Converter(GpuBgraToNv12Converter&&) noexcept = default;
  GpuBgraToNv12Converter& operator=(GpuBgraToNv12Converter&&) noexcept =
      default;

  GpuBgraToNv12ConvertResult Initialize(ID3D11Device* device,
                                        ID3D11DeviceContext* context,
                                        const GpuBgraToNv12Config& config);
  GpuBgraToNv12ConvertResult Convert(ID3D11Texture2D* source_bgra,
                                     ID3D11Texture2D* output_nv12);

  const GpuBgraToNv12Plan& plan() const noexcept { return plan_; }
  const GpuBgraToNv12ConverterStats& stats() const noexcept {
    return stats_;
  }
  bool IsInitialized() const noexcept;

 private:
  struct CachedInputView {
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11VideoProcessorInputView> view;
  };

  static constexpr size_t kMaximumCachedInputViewCount = 16;

  GpuBgraToNv12Plan plan_{};
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11VideoDevice> video_device_;
  winrt::com_ptr<ID3D11VideoContext> video_context_;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
  winrt::com_ptr<ID3D11VideoProcessor> processor_;
  std::vector<CachedInputView> input_views_;
  size_t next_input_view_eviction_index_ = 0;
  winrt::com_ptr<ID3D11Texture2D> output_view_texture_;
  winrt::com_ptr<ID3D11VideoProcessorOutputView> output_view_;
  GpuBgraToNv12ConverterStats stats_{};
};

bool BuildGpuBgraToNv12Plan(const GpuBgraToNv12Config& config,
                            GpuBgraToNv12Plan* plan,
                            std::wstring* error);

GpuBgraToNv12SmokeResult CreateGpuBgraToNv12SmokeTextures(
    ID3D11Device* device,
    const GpuBgraToNv12Config& config);

GpuBgraToNv12SmokeResult RunGpuBgraToNv12VideoProcessorSmoke(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const GpuBgraToNv12Config& config);

const wchar_t* GpuBgraToNv12StatusName(
    GpuBgraToNv12Status status) noexcept;

}  // namespace olouie::graphics
