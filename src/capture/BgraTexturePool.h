#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <memory>
#include <string>

#include "graphics/D3D11DeviceFault.h"

namespace olouie::capture {

struct BgraTexturePoolState;

struct BgraTexturePoolConfig {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t capacity = 0;

  bool IsValid() const noexcept;
};

struct BgraTexturePoolStats {
  uint32_t capacity = 0;
  uint32_t allocated_texture_count = 0;
  uint32_t available_texture_count = 0;
  uint32_t in_use_texture_count = 0;
  uint32_t peak_in_use_texture_count = 0;
  uint64_t acquire_count = 0;
  uint64_t created_texture_count = 0;
  uint64_t reused_texture_count = 0;
  uint64_t returned_texture_count = 0;
  uint64_t exhausted_count = 0;
  uint64_t creation_failure_count = 0;
};

class BgraTexturePoolLease final {
 public:
  BgraTexturePoolLease() = default;
  ~BgraTexturePoolLease();

  BgraTexturePoolLease(const BgraTexturePoolLease&) = delete;
  BgraTexturePoolLease& operator=(const BgraTexturePoolLease&) = delete;
  BgraTexturePoolLease(BgraTexturePoolLease&& other) noexcept;
  BgraTexturePoolLease& operator=(BgraTexturePoolLease&& other) noexcept;

  ID3D11Texture2D* get() const noexcept;
  bool IsValid() const noexcept;
  void Reset() noexcept;

 private:
  BgraTexturePoolLease(std::shared_ptr<BgraTexturePoolState> state,
                       winrt::com_ptr<ID3D11Texture2D> texture) noexcept;

  std::shared_ptr<BgraTexturePoolState> state_;
  winrt::com_ptr<ID3D11Texture2D> texture_;

  friend class BgraTexturePool;
};

enum class BgraTexturePoolAcquireStatus {
  Success,
  NotInitialized,
  Exhausted,
  TextureCreateFailed,
};

struct BgraTexturePoolAcquireResult {
  BgraTexturePoolAcquireStatus status =
      BgraTexturePoolAcquireStatus::NotInitialized;
  std::wstring message;
  BgraTexturePoolLease lease;
  HRESULT hresult = S_OK;
  graphics::D3D11DeviceFault device_fault;

  bool Succeeded() const noexcept;
};

class BgraTexturePool final {
 public:
  bool Initialize(ID3D11Device* device,
                  const BgraTexturePoolConfig& config,
                  std::wstring* error);
  void Reset() noexcept;

  bool IsInitialized() const noexcept;
  BgraTexturePoolAcquireResult Acquire();
  BgraTexturePoolStats stats() const;

 private:
  std::shared_ptr<BgraTexturePoolState> state_;
};

const wchar_t* BgraTexturePoolAcquireStatusName(
    BgraTexturePoolAcquireStatus status) noexcept;

}  // namespace olouie::capture
