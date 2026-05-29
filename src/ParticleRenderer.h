// ======================================
// File: ParticleRenderer.h
// Purpose: DX12 particle renderer module. Batches all particles from an Emitter
//          into billboard quads and draws them with additive blending.
//          Follows the same modular pattern as MeshRenderer.
// ======================================

#pragma once

#include "particle.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

class DxContext;

class ParticleRenderer {
public:
  static constexpr uint32_t kMaxParticles = 2048;

  void Initialize(DxContext &dx);
  void DrawParticles(DxContext &dx, const std::vector<const Emitter*> &emitters,
                     const DirectX::XMMATRIX &view,
                     const DirectX::XMMATRIX &proj);
  std::string ReloadShaders(DxContext &dx);
  void Reset();

private:
  void CreatePipeline(DxContext &dx);
  void CreateBuffers(DxContext &dx);

  // Per-particle vertex layout (4 vertices per particle quad).
  struct ParticleVertex {
    DirectX::XMFLOAT3 position; // world-space (billboard-expanded)
    DirectX::XMFLOAT2 uv;      // quad texture coordinate
    DirectX::XMFLOAT4 color;   // RGBA with alpha
  };

  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

  // Per-frame dynamic vertex buffer (upload heap, updated each frame).
  static constexpr uint32_t kFrameCount = 2;
  struct FrameVB {
    Microsoft::WRL::ComPtr<ID3D12Resource> vb;
    uint8_t *mapped = nullptr;
  };
  std::array<FrameVB, kFrameCount> m_frameVBs;

  // Static index buffer (quad indices, pre-built for max particles).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
  D3D12_INDEX_BUFFER_VIEW m_ibView{};

  bool m_initialized = false;
};
