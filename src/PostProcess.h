// ======================================
// File: PostProcess.h
// Purpose: Post-processing pipeline (Phase 9): Bloom, ACES Tonemapping, FXAA.
// ======================================

#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <string>
#include <wrl.h>

class DxContext;

struct PostProcessParams {
  float exposure = 1.0f;
  float bloomThreshold = 1.0f;
  float bloomIntensity = 0.5f;
  bool bloomEnabled = true;
  bool fxaaEnabled = true;

  // SSAO (Phase 10.3): AO applied during tonemap.
  float aoStrength = 0.0f;
  D3D12_GPU_DESCRIPTOR_HANDLE aoSrvGpu = {};

  // Motion blur (Phase 10.5)
  bool motionBlurEnabled = false;
  float motionBlurStrength = 1.0f;
  int motionBlurSamples = 8;

  // Depth of Field (Phase 10.6)
  bool dofEnabled = false;
  float dofFocalDistance = 10.0f;
  float dofFocalRange = 5.0f;
  float dofMaxBlur = 8.0f;
  float nearZ = 0.1f;
  float farZ = 1000.0f;

  // TAA (Phase 10.4)
  bool taaEnabled = false;
  float taaBlendFactor = 0.05f;
  D3D12_GPU_DESCRIPTOR_HANDLE hdrOverrideSrvGpu = {}; // when TAA is on, bloom/tonemap read this
};

class PostProcessRenderer {
public:
  void Initialize(DxContext &dx);
  void ExecuteBloom(DxContext &dx, const PostProcessParams &params);
  void ExecuteTonemap(DxContext &dx, const PostProcessParams &params);
  void ExecuteFXAA(DxContext &dx, const PostProcessParams &params);

  // Motion blur (Phase 10.5)
  void ExecuteVelocityGen(DxContext &dx,
                          const DirectX::XMMATRIX &invViewProj,
                          const DirectX::XMMATRIX &prevViewProj);
  void ExecuteMotionBlur(DxContext &dx, const PostProcessParams &params);

  // Depth of Field (Phase 10.6)
  void ExecuteDOF(DxContext &dx, const PostProcessParams &params);

  // TAA (Phase 10.4)
  void ExecuteTAA(DxContext &dx, const PostProcessParams &params);

  std::string ReloadShaders(DxContext &dx);
  void Reset();

private:
  // Bloom
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_bloomRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomDownPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomUpPso;

  // Tonemap
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_tonemapRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_tonemapPso;

  // FXAA
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fxaaRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fxaaPso;

  // Velocity generation (Phase 10.5)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_velocityRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_velocityPso;

  // Motion blur (Phase 10.5)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_motionBlurRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_motionBlurPso;

  // DOF (Phase 10.6)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_dofRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_dofPso;

  // TAA (Phase 10.4)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_taaRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_taaPso;
};
