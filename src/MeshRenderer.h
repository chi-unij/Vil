// ======================================
// File: MeshRenderer.h
// Purpose: Mesh rendering module (root signature + PSO + GPU buffers + texture
//          upload + draw). Keeps mesh logic out of DxContext.cpp.
// ======================================

#pragma once

#include "AnimationPlayer.h"
#include "GltfLoader.h"
#include "Lighting.h"
#include "ShadowMap.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

class DxContext;

class MeshRenderer {
public:
  uint32_t CreateMeshResources(DxContext &dx, const LoadedMesh &mesh,
                               const MaterialImages &images = {},
                               const Material &material = {});

  void DrawMesh(DxContext &dx, uint32_t meshId, const DirectX::XMMATRIX &world,
                const DirectX::XMMATRIX &view, const DirectX::XMMATRIX &proj,
                const LightParams &lighting,
                const MeshShadowParams &shadow);

  // Access per-mesh material for ImGui editing.
  Material &GetMeshMaterial(uint32_t meshId);
  const Material &GetMeshMaterial(uint32_t meshId) const;

  // Replace a single texture slot on an existing mesh (editor texture assignment).
  void ReplaceMeshTexture(DxContext &dx, uint32_t meshId, uint32_t slot,
                          const LoadedImage &img);
  // Clear a texture slot back to default.
  void ClearMeshTexture(DxContext &dx, uint32_t meshId, uint32_t slot);
  // Get ImGui-compatible GPU descriptor for a texture thumbnail.
  D3D12_GPU_DESCRIPTOR_HANDLE GetTextureImGuiSrv(DxContext &dx,
                                                  uint32_t meshId,
                                                  uint32_t slot);

  // G-buffer pass: write surface properties to MRT (Phase 12.1).
  void DrawMeshGBuffer(DxContext &dx, uint32_t meshId,
                       const DirectX::XMMATRIX &world,
                       const DirectX::XMMATRIX &view,
                       const DirectX::XMMATRIX &proj,
                       const DirectX::XMFLOAT3 &cameraPos);

  void DrawMeshShadow(DxContext &dx, uint32_t meshId,
                      const DirectX::XMMATRIX &world,
                      const DirectX::XMMATRIX &lightViewProj);

  // Instanced draw (Phase 12.5): draw all instances of a mesh in one call.
  void DrawMeshGBufferInstanced(DxContext &dx, uint32_t meshId,
                                const std::vector<DirectX::XMMATRIX> &worlds,
                                const DirectX::XMMATRIX &view,
                                const DirectX::XMMATRIX &proj,
                                const DirectX::XMFLOAT3 &cameraPos,
                                float gameTime = 0.0f);

  void DrawMeshShadowInstanced(DxContext &dx, uint32_t meshId,
                               const std::vector<DirectX::XMMATRIX> &worlds,
                               const DirectX::XMMATRIX &lightViewProj);

  void DrawMeshInstanced(DxContext &dx, uint32_t meshId,
                         const std::vector<DirectX::XMMATRIX> &worlds,
                         const DirectX::XMMATRIX &view,
                         const DirectX::XMMATRIX &proj,
                         const LightParams &lighting,
                         const MeshShadowParams &shadow,
                         float gameTime = 0.0f);

  void SetIBLDescriptors(D3D12_GPU_DESCRIPTOR_HANDLE iblTableBase);

  // Wireframe highlight (editor selection).
  void DrawMeshWireframe(DxContext &dx, uint32_t meshId,
                         const DirectX::XMMATRIX &world,
                         const DirectX::XMMATRIX &view,
                         const DirectX::XMMATRIX &proj,
                         const DirectX::XMFLOAT4 &color);

  // Set bone palette for a skinned mesh (call each frame before rendering).
  void SetBonePalette(uint32_t meshId, const BonePalette &palette);

  // Hot-reload all shaders, recreate PSOs. Returns empty on success, error string on failure.
  std::string ReloadShaders(DxContext &dx);

  void Reset(); // release all meshes + pipeline

private:
  struct MeshGpuResources;

  void CreatePipelineOnce(DxContext &dx);
  void CreateGBufferPipelineOnce(DxContext &dx);
  void CreateShadowPipelineOnce(DxContext &dx);
  void CreateWireframePipelineOnce(DxContext &dx);
  void CreateDefaultTextures(DxContext &dx);
  void CreateTextureResource(DxContext &dx,
                             Microsoft::WRL::ComPtr<ID3D12Resource> &outTex,
                             Microsoft::WRL::ComPtr<ID3D12Resource> &outUpload,
                             D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                             DXGI_FORMAT srvFormat,
                             const LoadedImage &img);
  void RebuildMaterialTable(DxContext &dx, uint32_t meshId);

private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gbufferRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gbufferPso;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowPso;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_wireframeRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_wireframePso;

  // Default 1x1 textures for missing material slots.
  // Store resource + format so we can create fresh SRV views per-slot.
  struct DefaultTex {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  };
  DefaultTex m_defaultWhite;
  DefaultTex m_defaultFlatNormal;
  DefaultTex m_defaultMetalRough;
  DefaultTex m_defaultMidGray; // 0.5 height for POM (no displacement)

  static constexpr uint32_t kMaterialTexCount = 6; // baseColor, normal, metalRough, AO, emissive, height

  struct MeshGpuResources {
    Microsoft::WRL::ComPtr<ID3D12Resource> vb;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
    uint32_t indexCount = 0;

    // Material textures (6 slots: baseColor, normal, metalRough, AO, emissive, height).
    Microsoft::WRL::ComPtr<ID3D12Resource> matTex[6];
    Microsoft::WRL::ComPtr<ID3D12Resource> matTexUpload[6];
    D3D12_GPU_DESCRIPTOR_HANDLE materialTableGpu{}; // contiguous 6-SRV block

    // Per-mesh material (Phase 11.5).
    Material material;

    // Cached ImGui SRV handles for texture thumbnails (Phase 2 material editor).
    D3D12_GPU_DESCRIPTOR_HANDLE matTexImGuiGpu[6] = {};
    bool matTexImGuiAllocated[6] = {};

    // Skeletal animation bone palette (Phase 2B/2C).
    BonePalette bonePalette;
  };

  std::vector<MeshGpuResources> m_meshes;

  D3D12_GPU_DESCRIPTOR_HANDLE m_iblTableGpu{};
};
