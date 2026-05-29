// ======================================
// File: GridRenderer.cpp
// Purpose: Grid floor + axis gizmo renderer implementation (extracted from
//          DxContext::CreateGridResources + DxContext::DrawGridAxes, Phase 8)
// ======================================

#include "GridRenderer.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"

#include <cstring>
#include <d3dcompiler.h>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

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
    ThrowIfFailed(hr, "D3DCompileFromFile failed");
  }

  return bytecode;
}

// Per-draw constant buffer layout (matches basic3d.hlsl).
struct SceneCB {
  DirectX::XMFLOAT4X4 worldViewProj;
};

void GridRenderer::Initialize(DxContext &dx) {
  // ---- Root signature: root CBV at b0 (vertex only) ----
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

  ComPtr<ID3DBlob> rsBlob;
  ComPtr<ID3DBlob> rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "D3D12SerializeRootSignature (grid) failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                 rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rootSig)),
                "CreateRootSignature (grid) failed");

  // ---- Line PSO ----
  auto vs = CompileShaderLocal(L"shaders/basic3d.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderLocal(L"shaders/basic3d.hlsl", "PSMain", "ps_5_0");

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  D3D12_BLEND_DESC blend{};
  blend.AlphaToCoverageEnable = FALSE;
  blend.IndependentBlendEnable = FALSE;
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].LogicOpEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;

  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_NONE;
  rast.FrontCounterClockwise = FALSE;
  rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
  rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
  rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
  rast.DepthClipEnable = TRUE;
  rast.MultisampleEnable = FALSE;
  rast.AntialiasedLineEnable = FALSE;
  rast.ForcedSampleCount = 0;
  rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  ds.StencilEnable = FALSE;
  pso.DepthStencilState = ds;
  pso.DSVFormat = dx.m_depthFormat;

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = dx.m_hdrFormat;
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(
      dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)),
      "CreateGraphicsPipelineState (grid/line) failed");

  // ---- Build grid + axis gizmo vertices ----
  struct Vertex {
    float px, py, pz;
    float r, g, b, a;
  };

  constexpr int halfLines = 20;
  constexpr float spacing = 1.0f;
  constexpr float y = 0.0f;

  const int gridLineCount = (2 * halfLines + 1) * 2;
  const int axisLineCount = 3;
  const int totalLineCount = gridLineCount + axisLineCount;
  const int totalVerts = totalLineCount * 2;

  std::vector<Vertex> verts;
  verts.reserve(static_cast<size_t>(totalVerts));

  auto pushLine = [&verts](float x0, float y0, float z0, float x1, float y1,
                           float z1, float r, float g, float b, float a) {
    verts.push_back(Vertex{x0, y0, z0, r, g, b, a});
    verts.push_back(Vertex{x1, y1, z1, r, g, b, a});
  };

  const float extent = halfLines * spacing;
  const float minor = 0.35f;
  const float major = 0.60f;

  for (int i = -halfLines; i <= halfLines; ++i) {
    const float v = i * spacing;
    const bool isMajor = (i % 5) == 0;
    const float c = isMajor ? major : minor;

    pushLine(-extent, y, v, extent, y, v, c, c, c, 1.0f);
    pushLine(v, y, -extent, v, y, extent, c, c, c, 1.0f);
  }

  // Axis gizmo at origin (X red, Y green, Z blue).
  const float axisLen = 2.5f;
  pushLine(0, 0, 0, axisLen, 0, 0, 1, 0, 0, 1);
  pushLine(0, 0, 0, 0, axisLen, 0, 0, 1, 0, 1);
  pushLine(0, 0, 0, 0, 0, axisLen, 0, 0, 1, 1);

  m_vertexCount = static_cast<uint32_t>(verts.size());
  const UINT vbSize = static_cast<UINT>(verts.size() * sizeof(Vertex));

  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC resDesc{};
  resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  resDesc.Width = vbSize;
  resDesc.Height = 1;
  resDesc.DepthOrArraySize = 1;
  resDesc.MipLevels = 1;
  resDesc.SampleDesc.Count = 1;
  resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&m_vb)),
                "CreateCommittedResource (GridVB) failed");

  void *vbMapped = nullptr;
  D3D12_RANGE range{0, 0};
  ThrowIfFailed(m_vb->Map(0, &range, &vbMapped), "GridVB Map failed");
  memcpy(vbMapped, verts.data(), vbSize);
  m_vb->Unmap(0, nullptr);

  m_vbView.BufferLocation = m_vb->GetGPUVirtualAddress();
  m_vbView.StrideInBytes = sizeof(Vertex);
  m_vbView.SizeInBytes = vbSize;
}

void GridRenderer::Draw(DxContext &dx, const DirectX::XMMATRIX &view,
                         const DirectX::XMMATRIX &proj) {
  using namespace DirectX;

  // Grid uses identity world transform.
  XMMATRIX world = XMMatrixIdentity();
  XMMATRIX wvp = world * view * proj;

  SceneCB cb{};
  XMStoreFloat4x4(&cb.worldViewProj, XMMatrixTranspose(wvp));
  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(SceneCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(cb));

  dx.SetViewportScissorFull();

  dx.m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());
  dx.m_cmdList->SetPipelineState(m_pso.Get());

  ID3D12DescriptorHeap *heaps[] = {dx.m_mainSrvHeap.Get()};
  dx.m_cmdList->SetDescriptorHeaps(1, heaps);
  dx.m_cmdList->SetGraphicsRootConstantBufferView(0, cbGpu);

  dx.m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
  dx.m_cmdList->IASetVertexBuffers(0, 1, &m_vbView);
  dx.m_cmdList->DrawInstanced(m_vertexCount, 1, 0, 0);
}

std::string GridRenderer::ReloadShaders(DxContext &dx) {
  std::string errors;
  if (!m_rootSig) return errors;
  auto vs = CompileShaderSafe(L"shaders/basic3d.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderSafe(L"shaders/basic3d.hlsl", "PSMain", "ps_5_0");
  if (vs.success && ps.success) {
    D3D12_INPUT_ELEMENT_DESC inputElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
    pso.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
    D3D12_BLEND_DESC blend{};
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
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState = ds;
    pso.DSVFormat = dx.m_depthFormat;
    pso.InputLayout = {inputElems, _countof(inputElems)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = dx.m_hdrFormat;
    pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> newPso;
    if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
      m_pso = newPso;
  } else {
    if (!vs.success) errors += "[basic3d.hlsl VS] " + vs.errorMessage + "\n";
    if (!ps.success) errors += "[basic3d.hlsl PS] " + ps.errorMessage + "\n";
  }
  return errors;
}

void GridRenderer::Reset() {
  m_pso.Reset();
  m_rootSig.Reset();
  m_vb.Reset();
  m_vbView = {};
  m_vertexCount = 0;
}
