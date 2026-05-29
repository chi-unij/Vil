// ======================================
// File: IBLGenerator.h
// Purpose: IBL precomputation — converts equirectangular HDRI into cubemap,
//          irradiance map, prefiltered specular map, and BRDF integration LUT.
//          All work is done on the GPU at init time.
// ======================================

#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

class DxContext;

class IBLGenerator {
public:
  void Initialize(DxContext &dx, ID3D12Resource *equirectHdri,
                  D3D12_GPU_DESCRIPTOR_HANDLE equirectSrvGpu);
  void Reset();

  // GPU handle at the start of the contiguous irradiance+prefiltered+brdfLut
  // SRV block (t2, t3, t4 for mesh shader).
  D3D12_GPU_DESCRIPTOR_HANDLE IBLTableGpuBase() const {
    return m_iblTableGpuBase;
  }

private:
  void CreateResources(DxContext &dx);
  void CreatePrecomputePipelines(DxContext &dx);
  void RunPrecomputation(DxContext &dx,
                         D3D12_GPU_DESCRIPTOR_HANDLE equirectSrvGpu);

  // Environment cubemap (intermediate).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_envCubemap;
  D3D12_GPU_DESCRIPTOR_HANDLE m_envCubeSrvGpu{};

  // Irradiance cubemap.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_irradianceCubemap;

  // Prefiltered specular cubemap.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_prefilteredCubemap;

  // BRDF LUT.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_brdfLut;

  // Contiguous SRV block base (irradiance, prefiltered, brdfLut).
  D3D12_GPU_DESCRIPTOR_HANDLE m_iblTableGpuBase{};

  // Precompute pipelines (released after init).
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_precomputeRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_equirectToCubePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_irradiancePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_prefilteredPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_brdfLutPso;

  // Temporary RTV heap for precomputation (released after init).
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_precomputeRtvHeap;
  uint32_t m_rtvDescriptorSize = 0;

  static constexpr uint32_t kEnvCubeSize = 512;
  static constexpr uint32_t kIrradianceSize = 32;
  static constexpr uint32_t kPrefilteredSize = 128;
  static constexpr uint32_t kPrefilteredMips = 5;
  static constexpr uint32_t kBrdfLutSize = 512;
  static constexpr DXGI_FORMAT kCubeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  static constexpr DXGI_FORMAT kBrdfFormat = DXGI_FORMAT_R16G16_FLOAT;
};
