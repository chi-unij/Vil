// ======================================
// File: SkyRenderer.h
// Purpose: HDRI sky background renderer (extracted from DxContext, Phase 8).
//          Owns sky root signature, PSO, HDRI texture, and per-frame CBs.
// ======================================

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <DirectXMath.h>

class DxContext;

class SkyRenderer {
public:
  void Initialize(DxContext &dx);
  void Draw(DxContext &dx, const DirectX::XMMATRIX &view,
            const DirectX::XMMATRIX &proj, float exposure);
  void Reset();

  std::string ReloadShaders(DxContext &dx);

  // Public accessors for IBL precomputation.
  ID3D12Resource *HdriTexture() const { return m_tex.Get(); }
  D3D12_GPU_DESCRIPTOR_HANDLE HdriSrvGpu() const { return m_srvGpu; }

private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_tex;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_texUpload;
  D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};

  static constexpr uint32_t kFrameCount = 2;
  struct FrameCB {
    Microsoft::WRL::ComPtr<ID3D12Resource> cb;
    uint8_t *mapped = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
  };
  std::array<FrameCB, kFrameCount> m_frameCBs;
};
