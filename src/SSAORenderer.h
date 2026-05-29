// ======================================
// File: SSAORenderer.h
// Purpose: Screen-Space Ambient Occlusion renderer (Phase 10.3).
//          Generates AO from depth + view-space normals, bilateral blur.
// ======================================

#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <string>
#include <wrl.h>

class DxContext;

class SSAORenderer {
public:
  void Initialize(DxContext &dx);
  void ExecuteSSAO(DxContext &dx, const DirectX::XMMATRIX &proj,
                   const DirectX::XMMATRIX &view,
                   float radius, float bias, float power, int kernelSize);
  void ExecuteBlur(DxContext &dx);
  std::string ReloadShaders(DxContext &dx);
  void Reset();

private:
  // SSAO generation pass
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ssaoRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ssaoPso;

  // Bilateral blur pass
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blurRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurPso;

  // 4x4 noise texture (random rotation vectors)
  Microsoft::WRL::ComPtr<ID3D12Resource> m_noiseTex;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_noiseUpload;
  D3D12_CPU_DESCRIPTOR_HANDLE m_noiseSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_noiseSrvGpu{};

  // Hemisphere kernel samples (pre-generated)
  static constexpr int kMaxKernelSize = 64;
  DirectX::XMFLOAT4 m_kernel[kMaxKernelSize];
};
