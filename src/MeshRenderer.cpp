// ======================================
// File: MeshRenderer.cpp
// Purpose: Mesh rendering implementation (PSO/RS creation, mesh uploads,
//          texture SRV creation, per-draw constants, draw calls).
//          Phase 11: PBR material textures (normal, metalRough, AO, emissive).
// ======================================

#include "MeshRenderer.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"

#include <cstring>
#include <d3dcompiler.h>
#include <stdexcept>

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

void MeshRenderer::Reset() {
  m_meshes.clear();
  m_pso.Reset();
  m_rootSig.Reset();
  m_shadowPso.Reset();
  m_shadowRootSig.Reset();
  m_wireframePso.Reset();
  m_wireframeRootSig.Reset();
  m_defaultWhite.tex.Reset();
  m_defaultWhite.upload.Reset();
  m_defaultFlatNormal.tex.Reset();
  m_defaultFlatNormal.upload.Reset();
  m_defaultMetalRough.tex.Reset();
  m_defaultMetalRough.upload.Reset();
  m_defaultMidGray.tex.Reset();
  m_defaultMidGray.upload.Reset();
}

std::string MeshRenderer::ReloadShaders(DxContext &dx) {
  std::string errors;

  // ---- Forward pass (mesh.hlsl) ----
  if (m_rootSig) {
    auto vs = CompileShaderSafe(L"shaders/mesh.hlsl", "VSMain", "vs_5_1");
    auto ps = CompileShaderSafe(L"shaders/mesh.hlsl", "PSMain", "ps_5_1");
    if (vs.success && ps.success) {
      D3D12_INPUT_ELEMENT_DESC inputElems[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_rootSig.Get();
      pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
      pso.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
      D3D12_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = FALSE;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      blend.RenderTarget[1].BlendEnable = FALSE;
      blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pso.BlendState = blend;
      pso.SampleMask = UINT_MAX;
      D3D12_RASTERIZER_DESC rast{};
      rast.FillMode = D3D12_FILL_MODE_SOLID;
      rast.CullMode = D3D12_CULL_MODE_BACK;
      rast.DepthClipEnable = TRUE;
      pso.RasterizerState = rast;
      D3D12_DEPTH_STENCIL_DESC ds{};
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      pso.DepthStencilState = ds;
      pso.DSVFormat = dx.m_depthFormat;
      pso.InputLayout = {inputElems, _countof(inputElems)};
      pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pso.NumRenderTargets = 2;
      pso.RTVFormats[0] = dx.m_hdrFormat;
      pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_pso = newPso;
    } else {
      if (!vs.success) errors += "[mesh.hlsl VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += "[mesh.hlsl PS] " + ps.errorMessage + "\n";
    }
  }

  // ---- G-buffer pass (gbuffer.hlsl) ----
  if (m_gbufferRootSig) {
    auto vs = CompileShaderSafe(L"shaders/gbuffer.hlsl", "VSMain", "vs_5_1");
    auto ps = CompileShaderSafe(L"shaders/gbuffer.hlsl", "PSMain", "ps_5_1");
    if (vs.success && ps.success) {
      D3D12_INPUT_ELEMENT_DESC inputElems[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_gbufferRootSig.Get();
      pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
      pso.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
      D3D12_BLEND_DESC blend{};
      for (int i = 0; i < 4; ++i) {
        blend.RenderTarget[i].BlendEnable = FALSE;
        blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      }
      pso.BlendState = blend;
      pso.SampleMask = UINT_MAX;
      D3D12_RASTERIZER_DESC rast{};
      rast.FillMode = D3D12_FILL_MODE_SOLID;
      rast.CullMode = D3D12_CULL_MODE_BACK;
      rast.DepthClipEnable = TRUE;
      pso.RasterizerState = rast;
      D3D12_DEPTH_STENCIL_DESC ds{};
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      pso.DepthStencilState = ds;
      pso.DSVFormat = dx.DepthFormat();
      pso.InputLayout = {inputElems, _countof(inputElems)};
      pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pso.NumRenderTargets = 4;
      pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
      pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pso.RTVFormats[3] = DXGI_FORMAT_R11G11B10_FLOAT;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_gbufferPso = newPso;
    } else {
      if (!vs.success) errors += "[gbuffer.hlsl VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += "[gbuffer.hlsl PS] " + ps.errorMessage + "\n";
    }
  }

  // ---- Shadow pass (shadow.hlsl) ----
  if (m_shadowRootSig) {
    auto vs = CompileShaderSafe(L"shaders/shadow.hlsl", "VSMain", "vs_5_1");
    if (vs.success) {
      D3D12_INPUT_ELEMENT_DESC inputElems[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_shadowRootSig.Get();
      pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
      pso.PS = {nullptr, 0};
      D3D12_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = FALSE;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pso.BlendState = blend;
      pso.SampleMask = UINT_MAX;
      D3D12_RASTERIZER_DESC rast{};
      rast.FillMode = D3D12_FILL_MODE_SOLID;
      rast.CullMode = D3D12_CULL_MODE_BACK;
      rast.DepthClipEnable = TRUE;
      rast.DepthBias = 1000;
      rast.SlopeScaledDepthBias = 1.0f;
      rast.DepthBiasClamp = 0.0f;
      pso.RasterizerState = rast;
      D3D12_DEPTH_STENCIL_DESC ds{};
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      pso.DepthStencilState = ds;
      pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      pso.InputLayout = {inputElems, _countof(inputElems)};
      pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pso.NumRenderTargets = 0;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_shadowPso = newPso;
    } else {
      errors += "[shadow.hlsl VS] " + vs.errorMessage + "\n";
    }
  }

  // ---- Wireframe pass (highlight.hlsl) ----
  if (m_wireframeRootSig) {
    auto vs = CompileShaderSafe(L"shaders/highlight.hlsl", "VSMain", "vs_5_1");
    auto ps = CompileShaderSafe(L"shaders/highlight.hlsl", "PSMain", "ps_5_1");
    if (vs.success && ps.success) {
      D3D12_INPUT_ELEMENT_DESC inputElems[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_wireframeRootSig.Get();
      pso.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
      pso.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
      D3D12_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = FALSE;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pso.BlendState = blend;
      pso.SampleMask = UINT_MAX;
      D3D12_RASTERIZER_DESC rast{};
      rast.FillMode = D3D12_FILL_MODE_WIREFRAME;
      rast.CullMode = D3D12_CULL_MODE_NONE;
      rast.DepthClipEnable = TRUE;
      rast.DepthBias = -100;
      pso.RasterizerState = rast;
      D3D12_DEPTH_STENCIL_DESC ds{};
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
      ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
      pso.DepthStencilState = ds;
      pso.DSVFormat = dx.DepthFormat();
      pso.InputLayout = {inputElems, _countof(inputElems)};
      pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pso.NumRenderTargets = 1;
      pso.RTVFormats[0] = dx.m_hdrFormat;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_wireframePso = newPso;
    } else {
      if (!vs.success) errors += "[highlight.hlsl VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += "[highlight.hlsl PS] " + ps.errorMessage + "\n";
    }
  }

  return errors;
}

void MeshRenderer::CreatePipelineOnce(DxContext &dx) {
  if (m_rootSig && m_pso)
    return;

  // Create default 1x1 textures first (needed for material table fallbacks).
  CreateDefaultTextures(dx);

  // Root Sig:
  // Param 0: root CBV at b0 (MeshCB)
  // Param 1: SRV table t0-t5 (6 material textures: baseColor, normal, metalRough, AO, emissive, height)
  // Param 2: SRV table t6 (shadow map)
  // Param 3: SRV table t7-t9 (IBL: irradiance, prefiltered, BRDF LUT)
  D3D12_DESCRIPTOR_RANGE matRange{};
  matRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  matRange.NumDescriptors = 6;
  matRange.BaseShaderRegister = 0; // t0-t5
  matRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE shadowRange{};
  shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  shadowRange.NumDescriptors = 1;
  shadowRange.BaseShaderRegister = 6; // t6
  shadowRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE iblRange{};
  iblRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  iblRange.NumDescriptors = 3;
  iblRange.BaseShaderRegister = 7; // t7, t8, t9
  iblRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[6]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0; // b0
  params[0].Descriptor.RegisterSpace = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &matRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &shadowRange;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 1;
  params[3].DescriptorTable.pDescriptorRanges = &iblRange;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Param 4: root SRV t10 — StructuredBuffer<float4x4> instance worlds (Phase 12.5).
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[4].Descriptor.ShaderRegister = 10; // t10
  params[4].Descriptor.RegisterSpace = 0;
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // Param 5: root SRV t11 — StructuredBuffer<float4x4> bone palette (Phase 2C skinning).
  params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[5].Descriptor.ShaderRegister = 11; // t11
  params[5].Descriptor.RegisterSpace = 0;
  params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_STATIC_SAMPLER_DESC samplers[3]{};
  // s0: material sampler (wrap, linear) — used for all material textures.
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[0].ShaderRegister = 0; // s0
  samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s1: shadow sampler (comparison, clamp)
  samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[1].ShaderRegister = 1; // s1
  samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s2: IBL sampler (linear, clamp) for cubemap + BRDF LUT sampling.
  samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[2].ShaderRegister = 2; // s2
  samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 6;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 3;
  rsDesc.pStaticSamplers = samplers;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize Mesh RS failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                 rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rootSig)),
                "Create Mesh RS failed");

  auto vs = CompileShaderLocal(L"shaders/mesh.hlsl", "VSMain", "vs_5_1");
  auto ps = CompileShaderLocal(L"shaders/mesh.hlsl", "PSMain", "ps_5_1");

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  // Standard Blend (MRT: 2 render targets)
  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blend.RenderTarget[1].BlendEnable = FALSE;
  blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_BACK;
  rast.DepthClipEnable = TRUE;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso.DepthStencilState = ds;
  pso.DSVFormat = dx.m_depthFormat;

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 2;
  pso.RTVFormats[0] = dx.m_hdrFormat;
  pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; // view-space normals (SSAO)
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)),
                "Create Mesh PSO failed");
}

// ============================================================================
// G-buffer pipeline (Phase 12.1 — Deferred Rendering).
// ============================================================================
void MeshRenderer::CreateGBufferPipelineOnce(DxContext &dx) {
  if (m_gbufferRootSig && m_gbufferPso)
    return;

  // Root Sig: param 0 = root CBV b0, param 1 = SRV table t0-t5 (material),
  //           param 2 = root SRV t6 (instance world matrices, vertex-only).
  D3D12_DESCRIPTOR_RANGE matRange{};
  matRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  matRange.NumDescriptors = 6;
  matRange.BaseShaderRegister = 0;
  matRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &matRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Param 2: root SRV t6 — StructuredBuffer<float4x4> instance worlds (Phase 12.5).
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[2].Descriptor.ShaderRegister = 6; // t6
  params[2].Descriptor.RegisterSpace = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // Param 3: root SRV t7 — StructuredBuffer<float4x4> bone palette (Phase 2C skinning).
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[3].Descriptor.ShaderRegister = 7; // t7
  params[3].Descriptor.RegisterSpace = 0;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 4;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize G-buffer RS failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                 rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_gbufferRootSig)),
                "Create G-buffer RS failed");

  auto vs = CompileShaderLocal(L"shaders/gbuffer.hlsl", "VSMain", "vs_5_1");
  auto ps = CompileShaderLocal(L"shaders/gbuffer.hlsl", "PSMain", "ps_5_1");

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_gbufferRootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  D3D12_BLEND_DESC blend{};
  for (int i = 0; i < 4; ++i) {
    blend.RenderTarget[i].BlendEnable = FALSE;
    blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_BACK;
  rast.DepthClipEnable = TRUE;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso.DepthStencilState = ds;
  pso.DSVFormat = dx.DepthFormat();

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 4;
  pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;     // albedo
  pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // normal
  pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;     // material
  pso.RTVFormats[3] = DXGI_FORMAT_R11G11B10_FLOAT;     // emissive
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(dx.m_device->CreateGraphicsPipelineState(&pso,
                                                         IID_PPV_ARGS(&m_gbufferPso)),
                "Create G-buffer PSO failed");
}

void MeshRenderer::DrawMeshGBuffer(DxContext &dx, uint32_t meshId,
                                   const DirectX::XMMATRIX &world,
                                   const DirectX::XMMATRIX &view,
                                   const DirectX::XMMATRIX &proj,
                                   const DirectX::XMFLOAT3 &cameraPos) {
  std::vector<DirectX::XMMATRIX> worlds = {world};
  DrawMeshGBufferInstanced(dx, meshId, worlds, view, proj, cameraPos);
}

void MeshRenderer::DrawMeshGBufferInstanced(
    DxContext &dx, uint32_t meshId,
    const std::vector<DirectX::XMMATRIX> &worlds,
    const DirectX::XMMATRIX &view, const DirectX::XMMATRIX &proj,
    const DirectX::XMFLOAT3 &cameraPos, float gameTime) {
  CreateGBufferPipelineOnce(dx);

  if (meshId >= m_meshes.size() || worlds.empty())
    return;
  const auto &mesh = m_meshes[meshId];
  if (mesh.indexCount == 0)
    return;

  auto cmd = dx.m_cmdList.Get();
  cmd->SetPipelineState(m_gbufferPso.Get());
  cmd->SetGraphicsRootSignature(m_gbufferRootSig.Get());

  // Per-batch constants (no per-object world — that's in the instance buffer).
  struct GBufferCB {
    DirectX::XMFLOAT4X4 viewMat;
    DirectX::XMFLOAT4X4 projMat;
    DirectX::XMFLOAT4 cameraPosV;
    DirectX::XMFLOAT4 materialFactors; // x=metallic, y=roughness
    DirectX::XMFLOAT4 emissiveFactor;
    DirectX::XMFLOAT4 pomParams;
    DirectX::XMFLOAT4 baseColorFactor; // rgba multiplier
    DirectX::XMFLOAT4 uvTilingOffset;  // xy=tiling, zw=offset
    DirectX::XMFLOAT4 animParams;      // x=gameTime, y=materialTypeId
  };

  GBufferCB cb{};
  DirectX::XMStoreFloat4x4(&cb.viewMat, DirectX::XMMatrixTranspose(view));
  DirectX::XMStoreFloat4x4(&cb.projMat, DirectX::XMMatrixTranspose(proj));
  cb.cameraPosV = {cameraPos.x, cameraPos.y, cameraPos.z, 0.0f};

  const auto &mat = mesh.material;
  cb.materialFactors = {mat.metallicFactor, mat.roughnessFactor, 0.0f, 0.0f};
  cb.emissiveFactor = {mat.emissiveFactor.x, mat.emissiveFactor.y,
                       mat.emissiveFactor.z, 0.0f};
  cb.pomParams = {mat.heightScale, mat.pomMinLayers, mat.pomMaxLayers,
                  mat.pomEnabled ? 1.0f : 0.0f};
  cb.baseColorFactor = mat.baseColorFactor;
  cb.uvTilingOffset = {mat.uvTiling.x, mat.uvTiling.y,
                       mat.uvOffset.x, mat.uvOffset.y};
  cb.animParams = {gameTime, mat.proceduralTypeId, 0.0f, 0.0f};

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(GBufferCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(GBufferCB));

  // Upload instance world matrices as StructuredBuffer (transposed for HLSL).
  uint32_t instCount = static_cast<uint32_t>(worlds.size());
  uint32_t instBytes = instCount * sizeof(DirectX::XMFLOAT4X4);
  void *instCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS instGpu =
      dx.AllocFrameConstants(instBytes, &instCpu);
  auto *dst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(instCpu);
  for (uint32_t i = 0; i < instCount; ++i)
    DirectX::XMStoreFloat4x4(&dst[i], DirectX::XMMatrixTranspose(worlds[i]));

  ID3D12DescriptorHeap *heaps[] = {dx.m_mainSrvHeap.Get()};
  cmd->SetDescriptorHeaps(1, heaps);

  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  if (mesh.materialTableGpu.ptr != 0)
    cmd->SetGraphicsRootDescriptorTable(1, mesh.materialTableGpu);
  cmd->SetGraphicsRootShaderResourceView(2, instGpu); // instance buffer

  // Upload bone palette — always upload kMaxBones to prevent out-of-range reads.
  {
    constexpr uint32_t boneBytes = kMaxBones * sizeof(DirectX::XMFLOAT4X4);
    void *boneCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS boneGpu =
        dx.AllocFrameConstants(boneBytes, &boneCpu);
    auto *boneDst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(boneCpu);
    if (mesh.bonePalette.boneCount > 0) {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(mesh.bonePalette.matrices[b]));
    } else {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
    cmd->SetGraphicsRootShaderResourceView(3, boneGpu);
  }

  dx.SetViewportScissorFull();
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
  cmd->IASetIndexBuffer(&mesh.ibView);
  cmd->DrawIndexedInstanced(mesh.indexCount, instCount, 0, 0, 0);
}

void MeshRenderer::CreateShadowPipelineOnce(DxContext &dx) {
  if (m_shadowRootSig && m_shadowPso)
    return;

  // Root Sig: param 0 = root CBV b0 (ShadowCB),
  //           param 1 = root SRV t0 (instance world matrices),
  //           param 2 = root SRV t1 (bone palette).
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0; // b0
  params[0].Descriptor.RegisterSpace = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // Param 1: root SRV t0 — StructuredBuffer<float4x4> instance worlds (Phase 12.5).
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister = 0; // t0
  params[1].Descriptor.RegisterSpace = 0;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // Param 2: root SRV t1 — StructuredBuffer<float4x4> bone palette (Phase 2C skinning).
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[2].Descriptor.ShaderRegister = 1; // t1
  params[2].Descriptor.RegisterSpace = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 3;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 0;
  rsDesc.pStaticSamplers = nullptr;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize Shadow RS failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&m_shadowRootSig)),
                "Create Shadow RS failed");

  auto vs = CompileShaderLocal(L"shaders/shadow.hlsl", "VSMain", "vs_5_1");

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_shadowRootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  // Depth-only: no PS, no RTV.
  pso.PS = {nullptr, 0};

  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_BACK;
  rast.DepthClipEnable = TRUE;
  // Conservative depth bias to reduce acne (tweakable later).
  rast.DepthBias = 1000;
  rast.SlopeScaledDepthBias = 1.0f;
  rast.DepthBiasClamp = 0.0f;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso.DepthStencilState = ds;
  pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 0;
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(dx.m_device->CreateGraphicsPipelineState(&pso,
                                                        IID_PPV_ARGS(&m_shadowPso)),
                "Create Shadow PSO failed");
}

// Create 1x1 default textures for missing material slots.
// We only create the GPU resource here — SRVs are created per-slot in
// CreateMeshResources using CreateShaderResourceView (avoids CopyDescriptorsSimple
// issues across heap types).
void MeshRenderer::CreateDefaultTextures(DxContext &dx) {
  struct DefaultTexDef {
    uint8_t rgba[4];
    DXGI_FORMAT srvFormat;
    DefaultTex *out;
  };

  // White (baseColor/AO/emissive fallback): full white, sRGB view for baseColor.
  // Flat normal: (128,128,255,255) = tangent-space (0,0,1).
  // MetalRough: (0,255,255,255) = neutral (G=1.0, B=1.0) so Material factors
  //   control the final value when no texture is present. (Phase 11.5)
  DefaultTexDef defs[] = {
      {{255, 255, 255, 255}, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &m_defaultWhite},
      {{128, 128, 255, 255}, DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultFlatNormal},
      {{0, 255, 255, 255},   DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultMetalRough},
      {{255, 255, 255, 255}, DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultMidGray},
  };

  for (auto &def : defs) {
    LoadedImage img;
    img.width = 1;
    img.height = 1;
    img.channels = 4;
    img.pixels = {def.rgba[0], def.rgba[1], def.rgba[2], def.rgba[3]};

    def.out->srvFormat = def.srvFormat;

    // Allocate a temporary SRV slot just to drive CreateTextureResource
    // (which creates the resource + uploads + creates SRV at a CPU handle).
    D3D12_CPU_DESCRIPTOR_HANDLE tmpSrv = dx.AllocMainSrvCpu(1);
    CreateTextureResource(dx, def.out->tex, def.out->upload, tmpSrv,
                          def.srvFormat, img);
  }
}

void MeshRenderer::CreateTextureResource(DxContext &dx,
                                         ComPtr<ID3D12Resource> &outTex,
                                         ComPtr<ID3D12Resource> &outUpload,
                                         D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                                         DXGI_FORMAT srvFormat,
                                         const LoadedImage &img) {
  // 1. Create Texture (Default Heap)
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = static_cast<UINT64>(img.width);
  tex.Height = static_cast<UINT>(img.height);
  tex.DepthOrArraySize = 1;
  tex.MipLevels = 1;
  tex.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  tex.SampleDesc.Count = 1;
  tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &defaultHeap, D3D12_HEAP_FLAG_NONE, &tex,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&outTex)),
                "Create Texture failed");

  // 2. Upload Buffer
  UINT64 uploadBufferSize = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  dx.m_device->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, &numRows,
                                     &rowSize, &uploadBufferSize);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc{};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = uploadBufferSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&outUpload)),
                "Create Texture Upload buffer failed");

  // 3. Copy Data (row-by-row)
  void *mappedTex = nullptr;
  outUpload->Map(0, nullptr, &mappedTex);
  const uint8_t *srcData = img.pixels.data();
  uint8_t *destData = reinterpret_cast<uint8_t *>(mappedTex);
  for (UINT i = 0; i < numRows; ++i) {
    memcpy(destData + footprint.Offset + i * footprint.Footprint.RowPitch,
           srcData + i * (img.width * 4), img.width * 4);
  }
  outUpload->Unmap(0, nullptr);

  // 4. Execute copy (blocking, simple).
  auto &alloc = dx.m_frames[0].cmdAlloc;
  ThrowIfFailed(alloc->Reset(), "CmdAlloc Reset failed");
  ThrowIfFailed(dx.m_cmdList->Reset(alloc.Get(), nullptr), "CmdList Reset failed");

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = outTex.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = outUpload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;

  dx.m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = outTex.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  dx.m_cmdList->ResourceBarrier(1, &barrier);

  ThrowIfFailed(dx.m_cmdList->Close(), "CmdList Close failed");
  ID3D12CommandList *lists[] = {dx.m_cmdList.Get()};
  dx.m_queue->ExecuteCommandLists(1, lists);
  dx.WaitForGpu();

  // 5. Create SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Format = srvFormat;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;

  dx.m_device->CreateShaderResourceView(outTex.Get(), &srvDesc, srvCpu);
}

uint32_t MeshRenderer::CreateMeshResources(DxContext &dx, const LoadedMesh &mesh,
                                           const MaterialImages &images,
                                           const Material &material) {
  if (mesh.vertices.empty() || mesh.indices.empty())
    return UINT32_MAX;

  CreatePipelineOnce(dx);

  MeshGpuResources gpu{};
  gpu.indexCount = static_cast<uint32_t>(mesh.indices.size());

  // Upload heaps for simplicity.
  const UINT vbSize =
      static_cast<UINT>(mesh.vertices.size() * sizeof(MeshVertex));
  const UINT ibSize = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  // VB
  D3D12_RESOURCE_DESC vbDesc{};
  vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  vbDesc.Width = vbSize;
  vbDesc.Height = 1;
  vbDesc.DepthOrArraySize = 1;
  vbDesc.MipLevels = 1;
  vbDesc.SampleDesc.Count = 1;
  vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&gpu.vb)),
                "Create Mesh VB failed");

  void *mapped = nullptr;
  gpu.vb->Map(0, nullptr, &mapped);
  memcpy(mapped, mesh.vertices.data(), vbSize);
  gpu.vb->Unmap(0, nullptr);

  gpu.vbView.BufferLocation = gpu.vb->GetGPUVirtualAddress();
  gpu.vbView.SizeInBytes = vbSize;
  gpu.vbView.StrideInBytes = sizeof(MeshVertex);

  // IB
  D3D12_RESOURCE_DESC ibDesc{};
  ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  ibDesc.Width = ibSize;
  ibDesc.Height = 1;
  ibDesc.DepthOrArraySize = 1;
  ibDesc.MipLevels = 1;
  ibDesc.SampleDesc.Count = 1;
  ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&gpu.ib)),
                "Create Mesh IB failed");

  gpu.ib->Map(0, nullptr, &mapped);
  memcpy(mapped, mesh.indices.data(), ibSize);
  gpu.ib->Unmap(0, nullptr);

  gpu.ibView.BufferLocation = gpu.ib->GetGPUVirtualAddress();
  gpu.ibView.SizeInBytes = ibSize;
  gpu.ibView.Format = DXGI_FORMAT_R32_UINT;

  // ---- Material textures (5 contiguous SRV slots) ----
  // Allocate 5 contiguous descriptors.
  D3D12_CPU_DESCRIPTOR_HANDLE matTableCpu = dx.AllocMainSrvCpu(kMaterialTexCount);
  gpu.materialTableGpu = dx.MainSrvGpuFromCpu(matTableCpu);

  const UINT srvIncr = dx.m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // Material slot definitions:
  // [0] baseColor (sRGB), [1] normal (linear), [2] metalRough (linear),
  // [3] AO (linear), [4] emissive (sRGB).
  struct MatSlot {
    const LoadedImage *img;
    DXGI_FORMAT format;
    const DefaultTex *defaultTex;
  };
  MatSlot slots[kMaterialTexCount] = {
      {images.baseColor,  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &m_defaultWhite},
      {images.normal,     DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultFlatNormal},
      {images.metalRough, DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultMetalRough},
      {images.ao,         DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultWhite},
      {images.emissive,   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &m_defaultWhite},
      {images.height,     DXGI_FORMAT_R8G8B8A8_UNORM,      &m_defaultMidGray},
  };

  for (uint32_t i = 0; i < kMaterialTexCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE slotCpu = matTableCpu;
    slotCpu.ptr += static_cast<SIZE_T>(i) * srvIncr;

    if (slots[i].img && !slots[i].img->pixels.empty()) {
      // Upload actual texture.
      CreateTextureResource(dx, gpu.matTex[i], gpu.matTexUpload[i],
                            slotCpu, slots[i].format, *slots[i].img);
    } else {
      // Create a fresh SRV pointing to the default texture resource.
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Format = slots[i].defaultTex->srvFormat;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Texture2D.MipLevels = 1;
      dx.m_device->CreateShaderResourceView(
          slots[i].defaultTex->tex.Get(), &srvDesc, slotCpu);
    }
  }

  // Store per-mesh material (Phase 11.5).
  gpu.material = material;
  // Set texture presence flags from what was actually uploaded.
  gpu.material.hasBaseColor  = (images.baseColor  && !images.baseColor->pixels.empty());
  gpu.material.hasNormal     = (images.normal     && !images.normal->pixels.empty());
  gpu.material.hasMetalRough = (images.metalRough && !images.metalRough->pixels.empty());
  gpu.material.hasAO         = (images.ao         && !images.ao->pixels.empty());
  gpu.material.hasEmissive   = (images.emissive   && !images.emissive->pixels.empty());
  gpu.material.hasHeight     = (images.height     && !images.height->pixels.empty());

  m_meshes.push_back(std::move(gpu));
  return static_cast<uint32_t>(m_meshes.size() - 1);
}

// ---- Texture replacement (Phase 2 — Material & Texture Editor) ----

static DXGI_FORMAT SlotSrvFormat(uint32_t slot) {
  // baseColor(0) and emissive(4) use sRGB; rest use linear UNORM.
  return (slot == 0 || slot == 4) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                  : DXGI_FORMAT_R8G8B8A8_UNORM;
}

void MeshRenderer::RebuildMaterialTable(DxContext &dx, uint32_t meshId) {
  auto &mesh = m_meshes[meshId];
  const UINT srvIncr = dx.m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_CPU_DESCRIPTOR_HANDLE tableCpu = dx.AllocMainSrvCpu(kMaterialTexCount);
  mesh.materialTableGpu = dx.MainSrvGpuFromCpu(tableCpu);

  const DefaultTex *defaults[kMaterialTexCount] = {
      &m_defaultWhite, &m_defaultFlatNormal, &m_defaultMetalRough,
      &m_defaultWhite, &m_defaultWhite, &m_defaultMidGray,
  };

  for (uint32_t i = 0; i < kMaterialTexCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE slotCpu = tableCpu;
    slotCpu.ptr += static_cast<SIZE_T>(i) * srvIncr;

    ID3D12Resource *res = mesh.matTex[i].Get();
    DXGI_FORMAT fmt = SlotSrvFormat(i);

    if (!res) {
      res = defaults[i]->tex.Get();
      fmt = defaults[i]->srvFormat;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = fmt;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    dx.m_device->CreateShaderResourceView(res, &srvDesc, slotCpu);
  }
}

void MeshRenderer::ReplaceMeshTexture(DxContext &dx, uint32_t meshId,
                                      uint32_t slot,
                                      const LoadedImage &img) {
  if (meshId >= m_meshes.size() || slot >= kMaterialTexCount)
    return;
  auto &mesh = m_meshes[meshId];

  // Upload new texture to a temporary SRV (will be overwritten by rebuild).
  D3D12_CPU_DESCRIPTOR_HANDLE tmpSrv = dx.AllocMainSrvCpu(1);
  CreateTextureResource(dx, mesh.matTex[slot], mesh.matTexUpload[slot],
                        tmpSrv, SlotSrvFormat(slot), img);

  // Update presence flag.
  bool *flags[] = {&mesh.material.hasBaseColor, &mesh.material.hasNormal,
                   &mesh.material.hasMetalRough, &mesh.material.hasAO,
                   &mesh.material.hasEmissive, &mesh.material.hasHeight};
  *flags[slot] = true;

  // Rebuild the contiguous 6-SRV table.
  RebuildMaterialTable(dx, meshId);

  // Invalidate ImGui thumbnail cache.
  mesh.matTexImGuiAllocated[slot] = false;
}

void MeshRenderer::ClearMeshTexture(DxContext &dx, uint32_t meshId,
                                    uint32_t slot) {
  if (meshId >= m_meshes.size() || slot >= kMaterialTexCount)
    return;
  auto &mesh = m_meshes[meshId];

  mesh.matTex[slot].Reset();
  mesh.matTexUpload[slot].Reset();

  bool *flags[] = {&mesh.material.hasBaseColor, &mesh.material.hasNormal,
                   &mesh.material.hasMetalRough, &mesh.material.hasAO,
                   &mesh.material.hasEmissive, &mesh.material.hasHeight};
  *flags[slot] = false;

  RebuildMaterialTable(dx, meshId);
  mesh.matTexImGuiAllocated[slot] = false;
}

D3D12_GPU_DESCRIPTOR_HANDLE
MeshRenderer::GetTextureImGuiSrv(DxContext &dx, uint32_t meshId,
                                 uint32_t slot) {
  D3D12_GPU_DESCRIPTOR_HANDLE null{};
  if (meshId >= m_meshes.size() || slot >= kMaterialTexCount)
    return null;

  auto &mesh = m_meshes[meshId];
  if (mesh.matTexImGuiAllocated[slot])
    return mesh.matTexImGuiGpu[slot];

  const DefaultTex *defaults[kMaterialTexCount] = {
      &m_defaultWhite, &m_defaultFlatNormal, &m_defaultMetalRough,
      &m_defaultWhite, &m_defaultWhite, &m_defaultMidGray,
  };

  ID3D12Resource *res = mesh.matTex[slot].Get();
  DXGI_FORMAT fmt = SlotSrvFormat(slot);
  if (!res) {
    res = defaults[slot]->tex.Get();
    fmt = defaults[slot]->srvFormat;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu;
  dx.ImGuiAllocSrv(&cpu, &gpu);

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Format = fmt;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  dx.m_device->CreateShaderResourceView(res, &srvDesc, cpu);

  mesh.matTexImGuiGpu[slot] = gpu;
  mesh.matTexImGuiAllocated[slot] = true;
  return gpu;
}

void MeshRenderer::DrawMesh(DxContext &dx, uint32_t meshId,
                            const DirectX::XMMATRIX &world,
                            const DirectX::XMMATRIX &view,
                            const DirectX::XMMATRIX &proj,
                            const LightParams &lighting,
                            const MeshShadowParams &shadow) {
  std::vector<DirectX::XMMATRIX> worlds = {world};
  DrawMeshInstanced(dx, meshId, worlds, view, proj, lighting, shadow);
}

void MeshRenderer::DrawMeshInstanced(
    DxContext &dx, uint32_t meshId,
    const std::vector<DirectX::XMMATRIX> &worlds,
    const DirectX::XMMATRIX &view, const DirectX::XMMATRIX &proj,
    const LightParams &lighting, const MeshShadowParams &shadow,
    float gameTime) {
  if (!m_pso || !m_rootSig)
    return;
  if (meshId >= m_meshes.size() || worlds.empty())
    return;
  const auto &mesh = m_meshes[meshId];
  if (mesh.indexCount == 0)
    return;

  auto cmd = dx.m_cmdList.Get();
  cmd->SetPipelineState(m_pso.Get());
  cmd->SetGraphicsRootSignature(m_rootSig.Get());

  // MRT: HDR color (RT0) + view-space normals (RT1) for SSAO.
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {dx.HdrRtv(), dx.ViewNormalRtv()};
  auto dsv = dx.Dsv();
  cmd->OMSetRenderTargets(2, rtvs, FALSE, &dsv);

  // Per-batch constants (world removed — per-instance now).
  struct MeshCB {
    DirectX::XMFLOAT4X4 viewMat;           // transposed
    DirectX::XMFLOAT4X4 projMat;           // transposed
    DirectX::XMFLOAT4 cameraPos;           // xyz
    DirectX::XMFLOAT4 lightDirIntensity;   // xyz + intensity
    DirectX::XMFLOAT4 lightColorRoughness; // rgb + roughness
    DirectX::XMFLOAT4 metallicPad;         // x = metallic, y = iblIntensity, z = cascadeDebug
    DirectX::XMFLOAT4X4 cascadeLightViewProj[kMaxCascades]; // transposed
    DirectX::XMFLOAT4 shadowParams;        // (texelX, texelY, bias, strength)
    DirectX::XMFLOAT4 cascadeSplits;       // xyz = split distances, w = cascadeCount
    DirectX::XMFLOAT4 emissiveFactor;      // rgb = emissive factor, w = unused
    DirectX::XMFLOAT4 pomParams;           // x = heightScale, y = minLayers, z = maxLayers, w = enabled
    DirectX::XMFLOAT4 baseColorFactor;     // rgba multiplier
    DirectX::XMFLOAT4 uvTilingOffset;      // xy=tiling, zw=offset
    DirectX::XMFLOAT4 animParams;          // x=gameTime, y=materialTypeId
  };

  MeshCB cb{};
  DirectX::XMStoreFloat4x4(&cb.viewMat, DirectX::XMMatrixTranspose(view));
  DirectX::XMStoreFloat4x4(&cb.projMat, DirectX::XMMatrixTranspose(proj));

  const DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
  const DirectX::XMVECTOR camPosV = invView.r[3];
  DirectX::XMStoreFloat4(&cb.cameraPos, camPosV);

  const auto &mat = mesh.material;

  cb.lightDirIntensity = {lighting.lightDir.x, lighting.lightDir.y,
                          lighting.lightDir.z, lighting.lightIntensity};
  cb.lightColorRoughness = {lighting.lightColor.x, lighting.lightColor.y,
                            lighting.lightColor.z, mat.roughnessFactor};
  cb.metallicPad = {mat.metallicFactor, lighting.iblIntensity,
                    lighting.cascadeDebug, 0.0f};

  for (uint32_t c = 0; c < shadow.cascadeCount && c < kMaxCascades; ++c) {
    DirectX::XMStoreFloat4x4(&cb.cascadeLightViewProj[c],
                             DirectX::XMMatrixTranspose(shadow.lightViewProj[c]));
  }
  cb.shadowParams = {shadow.texelSize.x, shadow.texelSize.y, shadow.bias,
                     shadow.strength};
  cb.cascadeSplits = {
      shadow.splitDistances[0], shadow.splitDistances[1],
      shadow.splitDistances[2], static_cast<float>(shadow.cascadeCount)};

  cb.emissiveFactor = {mat.emissiveFactor.x, mat.emissiveFactor.y,
                       mat.emissiveFactor.z, 0.0f};

  cb.pomParams = {mat.heightScale, mat.pomMinLayers,
                  mat.pomMaxLayers, mat.pomEnabled ? 1.0f : 0.0f};
  cb.baseColorFactor = mat.baseColorFactor;
  cb.uvTilingOffset = {mat.uvTiling.x, mat.uvTiling.y,
                       mat.uvOffset.x, mat.uvOffset.y};
  cb.animParams = {gameTime, mat.proceduralTypeId, 0.0f, 0.0f};

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(MeshCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(MeshCB));

  // Upload instance world matrices (transposed for HLSL).
  uint32_t instCount = static_cast<uint32_t>(worlds.size());
  uint32_t instBytes = instCount * sizeof(DirectX::XMFLOAT4X4);
  void *instCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS instGpu =
      dx.AllocFrameConstants(instBytes, &instCpu);
  auto *dst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(instCpu);
  for (uint32_t i = 0; i < instCount; ++i)
    DirectX::XMStoreFloat4x4(&dst[i], DirectX::XMMatrixTranspose(worlds[i]));

  ID3D12DescriptorHeap *heaps[] = {dx.m_mainSrvHeap.Get()};
  cmd->SetDescriptorHeaps(1, heaps);

  // Root param 0 = root CBV (b0),
  // Root param 1 = SRV table (t0-t5: material textures + height),
  // Root param 2 = SRV table (t6: shadow map),
  // Root param 3 = SRV table (t7-t9: IBL),
  // Root param 4 = root SRV (t10: instance worlds),
  // Root param 5 = root SRV (t11: bone palette).
  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  if (mesh.materialTableGpu.ptr != 0)
    cmd->SetGraphicsRootDescriptorTable(1, mesh.materialTableGpu);
  if (shadow.shadowSrvGpu.ptr != 0)
    cmd->SetGraphicsRootDescriptorTable(2, shadow.shadowSrvGpu);
  if (m_iblTableGpu.ptr != 0)
    cmd->SetGraphicsRootDescriptorTable(3, m_iblTableGpu);
  cmd->SetGraphicsRootShaderResourceView(4, instGpu); // instance buffer

  // Upload bone palette — always upload kMaxBones to prevent out-of-range reads.
  {
    constexpr uint32_t boneBytes = kMaxBones * sizeof(DirectX::XMFLOAT4X4);
    void *boneCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS boneGpu =
        dx.AllocFrameConstants(boneBytes, &boneCpu);
    auto *boneDst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(boneCpu);
    if (mesh.bonePalette.boneCount > 0) {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(mesh.bonePalette.matrices[b]));
    } else {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
    cmd->SetGraphicsRootShaderResourceView(5, boneGpu);
  }

  cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
  cmd->IASetIndexBuffer(&mesh.ibView);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawIndexedInstanced(mesh.indexCount, instCount, 0, 0, 0);
}

void MeshRenderer::DrawMeshShadow(DxContext &dx, uint32_t meshId,
                                  const DirectX::XMMATRIX &world,
                                  const DirectX::XMMATRIX &lightViewProj) {
  std::vector<DirectX::XMMATRIX> worlds = {world};
  DrawMeshShadowInstanced(dx, meshId, worlds, lightViewProj);
}

void MeshRenderer::DrawMeshShadowInstanced(
    DxContext &dx, uint32_t meshId,
    const std::vector<DirectX::XMMATRIX> &worlds,
    const DirectX::XMMATRIX &lightViewProj) {
  CreateShadowPipelineOnce(dx);
  if (!m_shadowPso || !m_shadowRootSig)
    return;
  if (meshId >= m_meshes.size() || worlds.empty())
    return;
  const auto &mesh = m_meshes[meshId];
  if (mesh.indexCount == 0)
    return;

  auto cmd = dx.m_cmdList.Get();
  cmd->SetPipelineState(m_shadowPso.Get());
  cmd->SetGraphicsRootSignature(m_shadowRootSig.Get());

  // Per-batch CB: just the lightViewProj (world is per-instance now).
  struct ShadowCB {
    DirectX::XMFLOAT4X4 lightViewProj; // transposed
  };

  ShadowCB cb{};
  DirectX::XMStoreFloat4x4(&cb.lightViewProj,
                           DirectX::XMMatrixTranspose(lightViewProj));

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(ShadowCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(ShadowCB));

  // Upload instance world matrices (transposed for HLSL).
  uint32_t instCount = static_cast<uint32_t>(worlds.size());
  uint32_t instBytes = instCount * sizeof(DirectX::XMFLOAT4X4);
  void *instCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS instGpu =
      dx.AllocFrameConstants(instBytes, &instCpu);
  auto *dst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(instCpu);
  for (uint32_t i = 0; i < instCount; ++i)
    DirectX::XMStoreFloat4x4(&dst[i], DirectX::XMMatrixTranspose(worlds[i]));

  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  cmd->SetGraphicsRootShaderResourceView(1, instGpu); // instance buffer

  // Upload bone palette — always upload kMaxBones to prevent out-of-range reads.
  {
    constexpr uint32_t boneBytes = kMaxBones * sizeof(DirectX::XMFLOAT4X4);
    void *boneCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS boneGpu =
        dx.AllocFrameConstants(boneBytes, &boneCpu);
    auto *boneDst = reinterpret_cast<DirectX::XMFLOAT4X4 *>(boneCpu);
    if (mesh.bonePalette.boneCount > 0) {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(mesh.bonePalette.matrices[b]));
    } else {
      for (int b = 0; b < kMaxBones; ++b)
        DirectX::XMStoreFloat4x4(&boneDst[b],
                                  DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
    cmd->SetGraphicsRootShaderResourceView(2, boneGpu);
  }

  cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
  cmd->IASetIndexBuffer(&mesh.ibView);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawIndexedInstanced(mesh.indexCount, instCount, 0, 0, 0);
}

void MeshRenderer::SetBonePalette(uint32_t meshId, const BonePalette &palette) {
  if (meshId < m_meshes.size())
    m_meshes[meshId].bonePalette = palette;
}

void MeshRenderer::SetIBLDescriptors(D3D12_GPU_DESCRIPTOR_HANDLE iblTableBase) {
  m_iblTableGpu = iblTableBase;
}

Material &MeshRenderer::GetMeshMaterial(uint32_t meshId) {
  return m_meshes[meshId].material;
}

const Material &MeshRenderer::GetMeshMaterial(uint32_t meshId) const {
  return m_meshes[meshId].material;
}

// ============================================================================
// Wireframe highlight pipeline (Milestone 4 — Editor selection).
// ============================================================================
void MeshRenderer::CreateWireframePipelineOnce(DxContext &dx) {
  if (m_wireframeRootSig && m_wireframePso)
    return;

  // Root Sig: param 0 = root CBV b0 (HighlightCB: WVP + color).
  D3D12_ROOT_PARAMETER param{};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  param.Descriptor.ShaderRegister = 0;
  param.Descriptor.RegisterSpace = 0;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 1;
  rsDesc.pParameters = &param;
  rsDesc.NumStaticSamplers = 0;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize Wireframe RS failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&m_wireframeRootSig)),
                "Create Wireframe RS failed");

  auto vs = CompileShaderLocal(L"shaders/highlight.hlsl", "VSMain", "vs_5_1");
  auto ps = CompileShaderLocal(L"shaders/highlight.hlsl", "PSMain", "ps_5_1");

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_wireframeRootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_WIREFRAME;
  rast.CullMode = D3D12_CULL_MODE_NONE; // show all edges
  rast.DepthClipEnable = TRUE;
  rast.DepthBias = -100; // slight bias toward camera so wireframe draws on top
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // don't write depth
  ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pso.DepthStencilState = ds;
  pso.DSVFormat = dx.DepthFormat();

  pso.InputLayout = {inputElems, _countof(inputElems)};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = dx.m_hdrFormat;
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(dx.m_device->CreateGraphicsPipelineState(
                    &pso, IID_PPV_ARGS(&m_wireframePso)),
                "Create Wireframe PSO failed");
}

void MeshRenderer::DrawMeshWireframe(DxContext &dx, uint32_t meshId,
                                     const DirectX::XMMATRIX &world,
                                     const DirectX::XMMATRIX &view,
                                     const DirectX::XMMATRIX &proj,
                                     const DirectX::XMFLOAT4 &color) {
  CreateWireframePipelineOnce(dx);

  if (meshId >= m_meshes.size())
    return;
  const auto &mesh = m_meshes[meshId];
  if (mesh.indexCount == 0)
    return;

  auto cmd = dx.m_cmdList.Get();
  cmd->SetPipelineState(m_wireframePso.Get());
  cmd->SetGraphicsRootSignature(m_wireframeRootSig.Get());

  struct HighlightCB {
    DirectX::XMFLOAT4X4 wvp;
    DirectX::XMFLOAT4 color;
  };

  HighlightCB cb{};
  DirectX::XMStoreFloat4x4(&cb.wvp,
                            DirectX::XMMatrixTranspose(world * view * proj));
  cb.color = color;

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(HighlightCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(HighlightCB));

  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);

  dx.SetViewportScissorFull();
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
  cmd->IASetIndexBuffer(&mesh.ibView);
  cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}
