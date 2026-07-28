#include "capture/BgraTexturePool.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace olouie::capture {

struct BgraTexturePoolState {
  mutable std::mutex mutex;
  winrt::com_ptr<ID3D11Device> device;
  BgraTexturePoolConfig config;
  std::vector<winrt::com_ptr<ID3D11Texture2D>> available;
  uint32_t allocated_slots = 0;
  BgraTexturePoolStats stats;

  void Return(winrt::com_ptr<ID3D11Texture2D> texture) noexcept {
    if (texture == nullptr) {
      return;
    }
    std::lock_guard lock(mutex);
    if (stats.in_use_texture_count > 0) {
      --stats.in_use_texture_count;
    }
    ++stats.returned_texture_count;
    available.push_back(std::move(texture));
    stats.available_texture_count =
        static_cast<uint32_t>(available.size());
  }
};

namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

BgraTexturePoolAcquireResult AcquireResult(
    BgraTexturePoolAcquireStatus status,
    std::wstring message = {},
    HRESULT hresult = S_OK,
    ID3D11Device* device = nullptr,
    std::wstring operation = {}) {
  BgraTexturePoolAcquireResult result;
  result.status = status;
  result.message = std::move(message);
  result.hresult = hresult;
  result.device_fault = graphics::InspectD3D11DeviceFault(
      device, hresult, std::move(operation));
  if (result.device_fault.Failed()) {
    result.message = result.device_fault.message;
  }
  return result;
}

HRESULT CreateTexture(const BgraTexturePoolState& state,
                      ID3D11Texture2D** texture) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = state.config.width;
  desc.Height = state.config.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  return state.device->CreateTexture2D(&desc, nullptr, texture);
}

}  // namespace

bool BgraTexturePoolConfig::IsValid() const noexcept {
  return width != 0 && height != 0 && capacity != 0;
}

BgraTexturePoolLease::BgraTexturePoolLease(
    std::shared_ptr<BgraTexturePoolState> state,
    winrt::com_ptr<ID3D11Texture2D> texture) noexcept
    : state_(std::move(state)), texture_(std::move(texture)) {}

BgraTexturePoolLease::~BgraTexturePoolLease() {
  Reset();
}

BgraTexturePoolLease::BgraTexturePoolLease(
    BgraTexturePoolLease&& other) noexcept
    : state_(std::move(other.state_)), texture_(std::move(other.texture_)) {}

BgraTexturePoolLease& BgraTexturePoolLease::operator=(
    BgraTexturePoolLease&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    texture_ = std::move(other.texture_);
  }
  return *this;
}

ID3D11Texture2D* BgraTexturePoolLease::get() const noexcept {
  return texture_.get();
}

bool BgraTexturePoolLease::IsValid() const noexcept {
  return state_ != nullptr && texture_ != nullptr;
}

void BgraTexturePoolLease::Reset() noexcept {
  if (state_ != nullptr && texture_ != nullptr) {
    state_->Return(std::move(texture_));
  }
  texture_ = nullptr;
  state_.reset();
}

bool BgraTexturePoolAcquireResult::Succeeded() const noexcept {
  return status == BgraTexturePoolAcquireStatus::Success && lease.IsValid();
}

bool BgraTexturePool::Initialize(ID3D11Device* device,
                                 const BgraTexturePoolConfig& config,
                                 std::wstring* error) {
  Reset();
  if (device == nullptr || !config.IsValid()) {
    SetError(error,
             L"BGRA texture pool needs a D3D11 device, dimensions, and "
             L"nonzero capacity.");
    return false;
  }

  auto state = std::make_shared<BgraTexturePoolState>();
  state->device.copy_from(device);
  state->config = config;
  state->available.reserve(config.capacity);
  state->stats.capacity = config.capacity;
  state_ = std::move(state);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void BgraTexturePool::Reset() noexcept {
  state_.reset();
}

bool BgraTexturePool::IsInitialized() const noexcept {
  return state_ != nullptr && state_->device != nullptr &&
         state_->config.IsValid();
}

BgraTexturePoolAcquireResult BgraTexturePool::Acquire() {
  const auto state = state_;
  if (state == nullptr || state->device == nullptr ||
      !state->config.IsValid()) {
    return AcquireResult(BgraTexturePoolAcquireStatus::NotInitialized,
                         L"BGRA texture pool is not initialized.");
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  bool create_texture = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->available.empty()) {
      texture = std::move(state->available.back());
      state->available.pop_back();
      ++state->stats.acquire_count;
      ++state->stats.reused_texture_count;
      ++state->stats.in_use_texture_count;
      state->stats.peak_in_use_texture_count = std::max(
          state->stats.peak_in_use_texture_count,
          state->stats.in_use_texture_count);
      state->stats.available_texture_count =
          static_cast<uint32_t>(state->available.size());
    } else if (state->allocated_slots < state->config.capacity) {
      ++state->allocated_slots;
      create_texture = true;
    } else {
      ++state->stats.exhausted_count;
      return AcquireResult(BgraTexturePoolAcquireStatus::Exhausted,
                           L"BGRA texture pool is exhausted.");
    }
  }

  if (create_texture) {
    const HRESULT hr = CreateTexture(*state, texture.put());
    if (FAILED(hr)) {
      {
        std::lock_guard lock(state->mutex);
        if (state->allocated_slots > 0) {
          --state->allocated_slots;
        }
        ++state->stats.creation_failure_count;
      }
      return AcquireResult(
          BgraTexturePoolAcquireStatus::TextureCreateFailed,
          L"Could not create a pooled app-owned BGRA texture.", hr,
          state->device.get(), L"WGC pooled BGRA texture creation");
    }

    std::lock_guard lock(state->mutex);
    ++state->stats.acquire_count;
    ++state->stats.created_texture_count;
    ++state->stats.in_use_texture_count;
    state->stats.allocated_texture_count = state->allocated_slots;
    state->stats.peak_in_use_texture_count = std::max(
        state->stats.peak_in_use_texture_count,
        state->stats.in_use_texture_count);
  }

  auto result = AcquireResult(BgraTexturePoolAcquireStatus::Success);
  result.lease = BgraTexturePoolLease(state, std::move(texture));
  return result;
}

BgraTexturePoolStats BgraTexturePool::stats() const {
  const auto state = state_;
  if (state == nullptr) {
    return {};
  }
  std::lock_guard lock(state->mutex);
  auto snapshot = state->stats;
  snapshot.allocated_texture_count = state->allocated_slots;
  snapshot.available_texture_count =
      static_cast<uint32_t>(state->available.size());
  return snapshot;
}

const wchar_t* BgraTexturePoolAcquireStatusName(
    BgraTexturePoolAcquireStatus status) noexcept {
  switch (status) {
    case BgraTexturePoolAcquireStatus::Success:
      return L"success";
    case BgraTexturePoolAcquireStatus::NotInitialized:
      return L"not initialized";
    case BgraTexturePoolAcquireStatus::Exhausted:
      return L"exhausted";
    case BgraTexturePoolAcquireStatus::TextureCreateFailed:
      return L"texture create failed";
  }
  return L"unknown";
}

}  // namespace olouie::capture
