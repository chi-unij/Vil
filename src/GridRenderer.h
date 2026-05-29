// ======================================
// File: GridRenderer.h
// Purpose: Grid floor + axis gizmo renderer (extracted from DxContext, Phase 8).
//          Owns its own root signature, line PSO, and grid VB.
// ======================================

#pragma once

#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <DirectXMath.h>

class DxContext;

class GridRenderer {
public:
  void Initialize(DxContext &dx);
  void Draw(DxContext &dx, const DirectX::XMMATRIX &view,
            const DirectX::XMMATRIX &proj);
  std::string ReloadShaders(DxContext &dx);
  void Reset();

private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_vb;
  D3D12_VERTEX_BUFFER_VIEW m_vbView{};
  uint32_t m_vertexCount = 0;
};
