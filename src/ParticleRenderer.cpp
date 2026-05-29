// ======================================
// File: ParticleRenderer.cpp
// Purpose: DX12 particle renderer implementation. Builds billboard quads on
//          CPU each frame and renders with additive alpha blending.
// ======================================

#include "ParticleRenderer.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"

#include <cstring>
#include <d3dcompiler.h>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static ComPtr<ID3DBlob> CompileShaderLocal(const wchar_t *filePath,
                                           const char *entryPoint,
                                           const char *target) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

  ComPtr<ID3DBlob> bytecode;
  ComPtr<ID3DBlob> errors;
  HRESULT hr =
      D3DCompileFromFile(filePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                         entryPoint, target, flags, 0, &bytecode, &errors);

  if (FAILED(hr)) {
    if (errors) {
      std::string msg((const char *)errors->GetBufferPointer(),
                      errors->GetBufferSize());
      throw std::runtime_error(msg);
    }
    ThrowIfFailed(hr, "D3DCompileFromFile (particle) failed");
  }

  return bytecode;
}

void ParticleRenderer::Reset() {
  for (auto &fvb : m_frameVBs) {
    if (fvb.vb && fvb.mapped) {
      fvb.vb->Unmap(0, nullptr);
      fvb.mapped = nullptr;
    }
    fvb.vb.Reset();
  }
  m_indexBuffer.Reset();
  m_pso.Reset();
  m_rootSig.Reset();
  m_initialized = false;
}

std::string ParticleRenderer::ReloadShaders(DxContext &dx) {
  std::string errors;
  if (!m_rootSig) return errors;
  auto vs = CompileShaderSafe(L"shaders/particle.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderSafe(L"shaders/particle.hlsl", "PSMain", "ps_5_0");
  if (vs.success && ps.success) {
    D3D12_INPUT_ELEMENT_DESC inputElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
    pso.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    pso.RasterizerState = rast;
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState = ds;
    pso.DSVFormat = dx.DepthFormat();
    pso.InputLayout = {inputElems, _countof(inputElems)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = dx.HdrFormat();
    pso.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> newPso;
    if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
      m_pso = newPso;
  } else {
    if (!vs.success) errors += "[particle.hlsl VS] " + vs.errorMessage + "\n";
    if (!ps.success) errors += "[particle.hlsl PS] " + ps.errorMessage + "\n";
  }
  return errors;
}

void ParticleRenderer::Initialize(DxContext &dx) {
  if (m_initialized)
    return;
  CreatePipeline(dx);
  CreateBuffers(dx);
  m_initialized = true;
}

void ParticleRenderer::CreatePipeline(DxContext &dx) {
  // Root signature: root CBV at b0 (ParticleCB with viewProj matrix).
  D3D12_ROOT_PARAMETER param{};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  param.Descriptor.ShaderRegister = 0; // b0
  param.Descriptor.RegisterSpace = 0;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 1;
  rsDesc.pParameters = &param;
  rsDesc.NumStaticSamplers = 0;
  rsDesc.pStaticSamplers = nullptr;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize Particle RS failed");
  ThrowIfFailed(dx.Device()->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&m_rootSig)),
                "Create Particle RS failed");

  // Compile particle shaders.
  auto vs = CompileShaderLocal(L"shaders/particle.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderLocal(L"shaders/particle.hlsl", "PSMain", "ps_5_0");

  // Input layout: POSITION (float3), TEXCOORD (float2), COLOR (float4).
  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  // Additive alpha blending for glow effect.
  D3D12_BLEND_DESC blend{};
  blend.AlphaToCoverageEnable = FALSE;
  blend.IndependentBlendEnable = FALSE;
  auto &rt0 = blend.RenderTarget[0];
  rt0.BlendEnable = TRUE;
  rt0.LogicOpEnable = FALSE;
  rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  rt0.DestBlend = D3D12_BLEND_ONE; // additive
  rt0.BlendOp = D3D12_BLEND_OP_ADD;
  rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
  rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
  rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  rt0.LogicOp = D3D12_LOGIC_OP_NOOP;
  rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;

  pso.SampleMask = UINT_MAX;

  // No backface culling (billboards).
  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_NONE;
  rast.FrontCounterClockwise = FALSE;
  rast.DepthClipEnable = TRUE;
  pso.RasterizerState = rast;

  // Depth test enabled (read) but depth write DISABLED (particles don't
  // occlude).
  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // read-only
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  ds.StencilEnable = FALSE;
  pso.DepthStencilState = ds;
  pso.DSVFormat = dx.DepthFormat();

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = dx.HdrFormat();
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(
      dx.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)),
      "Create Particle PSO failed");
}

void ParticleRenderer::CreateBuffers(DxContext &dx) {
  // Dynamic vertex buffers (one per frame, upload heap, persistently mapped).
  const UINT vbSize = kMaxParticles * 4 * sizeof(ParticleVertex);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC vbDesc{};
  vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  vbDesc.Width = vbSize;
  vbDesc.Height = 1;
  vbDesc.DepthOrArraySize = 1;
  vbDesc.MipLevels = 1;
  vbDesc.SampleDesc.Count = 1;
  vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  D3D12_RANGE readRange{0, 0};
  for (uint32_t i = 0; i < kFrameCount; ++i) {
    ThrowIfFailed(dx.Device()->CreateCommittedResource(
                      &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                      IID_PPV_ARGS(&m_frameVBs[i].vb)),
                  "Create Particle VB failed");
    ThrowIfFailed(
        m_frameVBs[i].vb->Map(0, &readRange,
                              reinterpret_cast<void **>(&m_frameVBs[i].mapped)),
        "Particle VB Map failed");
  }

  // Static index buffer with quad indices for max particles.
  // Each quad: 0,1,2, 2,1,3 (two triangles).
  std::vector<uint16_t> indices(kMaxParticles * 6);
  for (uint32_t i = 0; i < kMaxParticles; ++i) {
    uint16_t base = static_cast<uint16_t>(i * 4);
    indices[i * 6 + 0] = base + 0;
    indices[i * 6 + 1] = base + 1;
    indices[i * 6 + 2] = base + 2;
    indices[i * 6 + 3] = base + 2;
    indices[i * 6 + 4] = base + 1;
    indices[i * 6 + 5] = base + 3;
  }

  const UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint16_t));

  D3D12_RESOURCE_DESC ibDesc{};
  ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  ibDesc.Width = ibSize;
  ibDesc.Height = 1;
  ibDesc.DepthOrArraySize = 1;
  ibDesc.MipLevels = 1;
  ibDesc.SampleDesc.Count = 1;
  ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(dx.Device()->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&m_indexBuffer)),
                "Create Particle IB failed");

  void *ibMapped = nullptr;
  ThrowIfFailed(m_indexBuffer->Map(0, &readRange, &ibMapped),
                "Particle IB Map failed");
  memcpy(ibMapped, indices.data(), ibSize);
  m_indexBuffer->Unmap(0, nullptr);

  m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
  m_ibView.SizeInBytes = ibSize;
  m_ibView.Format = DXGI_FORMAT_R16_UINT;
}

void ParticleRenderer::DrawParticles(DxContext &dx,
                                     const std::vector<const Emitter*> &emitters,
                                     const DirectX::XMMATRIX &view,
                                     const DirectX::XMMATRIX &proj) {
  if (!m_initialized)
    return;

  // Count total particles across all emitters, capped at kMaxParticles.
  uint32_t totalCount = 0;
  for (const auto *em : emitters) {
    if (!em) continue;
    totalCount += static_cast<uint32_t>(em->GetCount());
  }
  if (totalCount == 0)
    return;

  const uint32_t drawCount = std::min(totalCount, kMaxParticles);

  // Extract camera right and up vectors from the view matrix for billboarding.
  XMMATRIX invView = XMMatrixInverse(nullptr, view);
  XMVECTOR camRight = invView.r[0];
  XMVECTOR camUp = invView.r[1];

  // Build billboard quads for all particles into the dynamic VB.
  const uint32_t frameIdx = dx.FrameIndex();
  auto *verts =
      reinterpret_cast<ParticleVertex *>(m_frameVBs[frameIdx].mapped);

  uint32_t offset = 0;
  for (const auto *em : emitters) {
    if (!em) continue;
    const size_t emCount = em->GetCount();
    for (size_t pi = 0; pi < emCount && offset < drawCount; ++pi, ++offset) {
      const ParticleVisual vis = em->GetParticle(pi)->GetVisual();
      const XMVECTOR center = XMLoadFloat3(&vis.position);
      const float s = vis.scale;

      XMVECTOR r = XMVectorScale(camRight, s);
      XMVECTOR u = XMVectorScale(camUp, s);

      XMVECTOR positions[4] = {
          XMVectorAdd(center, XMVectorSubtract(u, r)),
          XMVectorAdd(center, XMVectorAdd(r, u)),
          XMVectorSubtract(center, XMVectorAdd(r, u)),
          XMVectorAdd(center, XMVectorSubtract(r, u)),
      };

      XMFLOAT2 uvs[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};

      for (int v = 0; v < 4; ++v) {
        ParticleVertex &vert = verts[offset * 4 + v];
        XMStoreFloat3(&vert.position, positions[v]);
        vert.uv = uvs[v];
        vert.color = vis.color;
      }
    }
  }

  // Upload constant buffer (ViewProj matrix).
  struct ParticleCB {
    XMFLOAT4X4 viewProj;
  };

  ParticleCB cb{};
  XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(view * proj));
  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(ParticleCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(ParticleCB));

  // Set up pipeline.
  auto cmd = dx.CmdList();

  // Ensure render targets are bound (HDR target for scene rendering).
  auto rtv = dx.HdrRtv();
  auto dsv = dx.Dsv();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

  cmd->SetGraphicsRootSignature(m_rootSig.Get());
  cmd->SetPipelineState(m_pso.Get());

  D3D12_VIEWPORT vp{};
  vp.Width = static_cast<float>(dx.Width());
  vp.Height = static_cast<float>(dx.Height());
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor{0, 0, static_cast<LONG>(dx.Width()),
                     static_cast<LONG>(dx.Height())};

  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &scissor);

  // Bind descriptor heap (needed even though we don't use SRVs, for
  // consistency with the rest of the engine).
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);

  // Root param 0 = CBV (viewProj).
  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);

  // Vertex buffer.
  const UINT vbSizeBytes =
      drawCount * 4 * static_cast<UINT>(sizeof(ParticleVertex));
  D3D12_VERTEX_BUFFER_VIEW vbView{};
  vbView.BufferLocation = m_frameVBs[frameIdx].vb->GetGPUVirtualAddress();
  vbView.SizeInBytes = vbSizeBytes;
  vbView.StrideInBytes = sizeof(ParticleVertex);

  cmd->IASetVertexBuffers(0, 1, &vbView);
  cmd->IASetIndexBuffer(&m_ibView);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cmd->DrawIndexedInstanced(drawCount * 6, 1, 0, 0, 0);
}
