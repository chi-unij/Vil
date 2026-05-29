// ======================================
// File: ShadowMap.h
// Purpose: Cascaded shadow map resource + pass helpers (Phase 10.1: CSM with
//          Texture2DArray, one slice per cascade)
// ======================================

#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <DirectXMath.h>

class DxContext;

static constexpr uint32_t kMaxCascades    = 4;
static constexpr uint32_t kDefaultCascades = 3;

// CPU-side parameters used when drawing shaded meshes with shadows.
// (This is NOT a GPU resource; it just bundles the knobs + descriptors.)
struct MeshShadowParams {
  uint32_t cascadeCount = 0;

  // One light view-projection per cascade (world -> light clip).
  std::array<DirectX::XMMATRIX, kMaxCascades> lightViewProj = {};

  // View-space far-plane distance for each cascade split.
  std::array<float, kMaxCascades> splitDistances = {};

  // (1 / shadowMapSize) for PCF sampling.
  DirectX::XMFLOAT2 texelSize = {0.0f, 0.0f};

  // Depth bias in light clip depth space [0..1]. Start small; tune per scene.
  float bias = 0.0015f;

  // 0 = no shadowing, 1 = full shadowing.
  float strength = 1.0f;

  // Shader-visible descriptor for the shadow Texture2DArray SRV.
  D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
};

class ShadowMap {
public:
  void Initialize(DxContext &dx, uint32_t size,
                  uint32_t cascadeCount = kDefaultCascades);

  // Per-cascade rendering. Caller loops cascades externally.
  // BeginCascade(0) transitions entire resource SRV -> DEPTH_WRITE.
  // Each call clears the cascade's DSV and sets viewport/scissor.
  void BeginCascade(DxContext &dx, uint32_t cascadeIndex);

  // Transitions entire resource DEPTH_WRITE -> SRV. Call once after all
  // cascades are rendered.
  void EndAllCascades(DxContext &dx);

  uint32_t Size() const { return m_size; }
  uint32_t CascadeCount() const { return m_cascadeCount; }
  DirectX::XMFLOAT2 TexelSize() const {
    const float inv = (m_size > 0) ? (1.0f / static_cast<float>(m_size)) : 0.0f;
    return {inv, inv};
  }

  D3D12_CPU_DESCRIPTOR_HANDLE Dsv(uint32_t cascade) const;
  D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu() const { return m_srvGpu; }

private:
  uint32_t m_size = 0;
  uint32_t m_cascadeCount = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_tex; // R32_TYPELESS Texture2DArray
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap; // N descriptors
  uint32_t m_dsvDescriptorSize = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};

  D3D12_VIEWPORT m_vp{};
  D3D12_RECT m_scissor{};
};
