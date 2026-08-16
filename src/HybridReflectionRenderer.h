// ======================================
// File: HybridReflectionRenderer.h
// Purpose: Hybrid DXR reflection capability and resource owner.
//          Unsupported environments retain the existing SSR path.
// ======================================

#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl.h>

class DxContext;
class MeshRenderer;
struct FrameData;

class HybridReflectionRenderer {
public:
  bool Initialize(DxContext &dx);
  bool PrepareScene(DxContext &dx, const FrameData &frame,
                    const MeshRenderer &meshRenderer);
  bool Execute(DxContext &dx, const FrameData &frame);
  void Reset();

  bool IsSupported() const { return m_supported; }
  bool ShaderAvailable() const { return m_shaderAvailable; }
  bool SceneReady() const { return m_sceneReady; }
  uint32_t SceneInstanceCount() const {
    return static_cast<uint32_t>(m_sceneInstances.size());
  }
  uint32_t BlasCount() const {
    return static_cast<uint32_t>(m_blasCache.size());
  }
  D3D12_GPU_DESCRIPTOR_HANDLE OutputSrvGpu() const {
    return m_outputSrvGpu;
  }
  D3D12_RAYTRACING_TIER RaytracingTier() const { return m_raytracingTier; }
  const std::string &Status() const { return m_status; }

private:
  struct BlasResources {
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
  };

  struct SceneInstance {
    uint32_t meshId = UINT32_MAX;
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMFLOAT4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
  };

  bool CreateProofPipeline(DxContext &dx);
  bool EnsureOutput(DxContext &dx);
  bool BuildBlas(DxContext &dx, uint32_t meshId,
                 const MeshRenderer &meshRenderer);
  void ResetScene();

  bool m_supported = false;
  bool m_shaderAvailable = false;
  bool m_sceneReady = false;
  D3D12_RAYTRACING_TIER m_raytracingTier =
      D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
  std::string m_status = "DXR は未初期化です。";

  Microsoft::WRL::ComPtr<ID3D12Device5> m_device5;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_commandList4;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_computePso;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_outputTexture;
  D3D12_CPU_DESCRIPTOR_HANDLE m_outputUavCpu{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_outputSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_outputUavGpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_outputSrvGpu{};
  uint32_t m_outputWidth = 0;
  uint32_t m_outputHeight = 0;
  bool m_outputDescriptorsAllocated = false;
  std::unordered_map<uint32_t, BlasResources> m_blasCache;
  std::vector<SceneInstance> m_sceneInstances;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasScratch;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasResult;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceUpload;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceColorUpload;
};
