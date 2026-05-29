// ======================================
// File: ShadowMap.cpp
// Purpose: Cascaded shadow map resource creation + per-cascade pass transitions
//          (Phase 10.1: Texture2DArray with one slice per cascade)
// ======================================

#include "ShadowMap.h"

#include "DxContext.h"
#include "DxUtil.h"

void ShadowMap::Initialize(DxContext &dx, uint32_t size, uint32_t cascadeCount) {
  m_size = size;
  m_cascadeCount = cascadeCount;

  // 1) Create DSV heap (one descriptor per cascade).
  D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{};
  dsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeap.NumDescriptors = cascadeCount;
  dsvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ThrowIfFailed(dx.m_device->CreateDescriptorHeap(&dsvHeap,
                                                  IID_PPV_ARGS(&m_dsvHeap)),
                "CreateDescriptorHeap (Shadow DSV) failed");
  m_dsvDescriptorSize =
      dx.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  // 2) Create depth Texture2DArray (one slice per cascade).
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = static_cast<UINT64>(m_size);
  tex.Height = static_cast<UINT>(m_size);
  tex.DepthOrArraySize = static_cast<UINT16>(cascadeCount);
  tex.MipLevels = 1;
  tex.Format = DXGI_FORMAT_R32_TYPELESS; // typeless so we can have DSV + SRV views
  tex.SampleDesc.Count = 1;
  tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_D32_FLOAT;
  clear.DepthStencil.Depth = 1.0f;
  clear.DepthStencil.Stencil = 0;

  // Start in SRV state so we can transition to DEPTH_WRITE at the start of the
  // frame (consistent BeginCascade/EndAllCascades transitions).
  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &tex,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                    IID_PPV_ARGS(&m_tex)),
                "CreateCommittedResource (ShadowMap Texture2DArray) failed");

  // 3) Per-cascade DSV views (each targets one array slice).
  for (uint32_t i = 0; i < cascadeCount; ++i) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv.Texture2DArray.MipSlice = 0;
    dsv.Texture2DArray.FirstArraySlice = i;
    dsv.Texture2DArray.ArraySize = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(i) * m_dsvDescriptorSize;
    dx.m_device->CreateDepthStencilView(m_tex.Get(), &dsv, handle);
  }

  // 4) Single SRV covering the entire Texture2DArray (all cascades).
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray.MostDetailedMip = 0;
  srv.Texture2DArray.MipLevels = 1;
  srv.Texture2DArray.FirstArraySlice = 0;
  srv.Texture2DArray.ArraySize = cascadeCount;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu = dx.AllocMainSrvCpu(1);
  dx.m_device->CreateShaderResourceView(m_tex.Get(), &srv, cpu);
  m_srvGpu = dx.MainSrvGpuFromCpu(cpu);

  // 5) Viewport/scissor (same for all cascades).
  m_vp.TopLeftX = 0.0f;
  m_vp.TopLeftY = 0.0f;
  m_vp.Width = static_cast<float>(m_size);
  m_vp.Height = static_cast<float>(m_size);
  m_vp.MinDepth = 0.0f;
  m_vp.MaxDepth = 1.0f;

  m_scissor.left = 0;
  m_scissor.top = 0;
  m_scissor.right = static_cast<LONG>(m_size);
  m_scissor.bottom = static_cast<LONG>(m_size);
}

void ShadowMap::BeginCascade(DxContext &dx, uint32_t cascadeIndex) {
  if (!m_tex || !m_dsvHeap)
    return;

  // Transition entire resource on first cascade only.
  if (cascadeIndex == 0) {
    dx.Transition(m_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }

  dx.m_cmdList->RSSetViewports(1, &m_vp);
  dx.m_cmdList->RSSetScissorRects(1, &m_scissor);

  D3D12_CPU_DESCRIPTOR_HANDLE dsv = Dsv(cascadeIndex);
  dx.m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
  dx.m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                      nullptr);
}

void ShadowMap::EndAllCascades(DxContext &dx) {
  if (!m_tex)
    return;

  // Transition depth write -> SRV for main pass.
  dx.Transition(m_tex.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowMap::Dsv(uint32_t cascade) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h =
      m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += static_cast<SIZE_T>(cascade) * m_dsvDescriptorSize;
  return h;
}
