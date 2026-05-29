// ======================================
// File: RenderPasses.cpp
// Purpose: Concrete render pass implementations (Phase 8). Each pass wraps
//          existing renderer modules and executes them with FrameData.
// ======================================

#include "RenderPasses.h"

#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"
#include "GridRenderer.h"
#include "ImGuiLayer.h"
#include "MeshRenderer.h"
#include "ParticleRenderer.h"
#include "PostProcess.h"
#include "SSAORenderer.h"
#include "ShadowMap.h"
#include "SkyRenderer.h"

#include <algorithm>
#include <d3dcompiler.h>
#include <unordered_map>

// ============================================================================
// BuildBatches — group RenderItems by meshId for instanced draw (Phase 12.5).
// ============================================================================
static std::vector<InstanceBatch>
BuildBatches(const std::vector<RenderItem> &items) {
  // Preserve ordering: first occurrence of a meshId determines batch position.
  std::unordered_map<uint32_t, size_t> meshToIndex;
  std::vector<InstanceBatch> batches;
  for (const auto &item : items) {
    if (item.meshId == UINT32_MAX)
      continue;
    auto it = meshToIndex.find(item.meshId);
    if (it == meshToIndex.end()) {
      meshToIndex[item.meshId] = batches.size();
      batches.push_back({item.meshId, {item.world}});
    } else {
      batches[it->second].worldMatrices.push_back(item.world);
    }
  }
  return batches;
}

// ============================================================================
// ShadowPass
// ============================================================================
void ShadowPass::Execute(DxContext &dx, const FrameData &frame) {
  if (!frame.shadowsEnabled)
    return;

  auto batches = BuildBatches(frame.opaqueItems);
  for (uint32_t c = 0; c < frame.cascadeCount; ++c) {
    m_shadow.BeginCascade(dx, c);
    for (const auto &batch : batches) {
      m_mesh.DrawMeshShadowInstanced(dx, batch.meshId, batch.worldMatrices,
                                     frame.cascadeLightViewProj[c]);
    }
  }
  m_shadow.EndAllCascades(dx);
}

// ============================================================================
// SkyPass — clears backbuffer + depth, then draws HDRI sky background.
// ============================================================================
void SkyPass::Execute(DxContext &dx, const FrameData &frame) {
  dx.Clear(frame.clearColor[0], frame.clearColor[1], frame.clearColor[2],
           frame.clearColor[3]);
  dx.ClearDepth(1.0f);

  // Clear view-normal RT to packed up-facing normal (0.5, 0.5, 1.0) so sky
  // pixels don't produce false SSAO occlusion.
  const float normalClear[4] = {0.5f, 0.5f, 1.0f, 0.0f};
  auto normalRtv = dx.ViewNormalRtv();
  dx.CmdList()->ClearRenderTargetView(normalRtv, normalClear, 0, nullptr);

  m_sky.Draw(dx, frame.view, frame.proj, frame.skyExposure);
}

// ============================================================================
// OpaquePass — grid/axes + lit meshes with shadow sampling.
// ============================================================================
void OpaquePass::Execute(DxContext &dx, const FrameData &frame) {
  // Draw grid/axes first (lines).
  m_grid.Draw(dx, frame.view, frame.proj);

  // Build shadow params from FrameData + ShadowMap state (CSM).
  MeshShadowParams shadowParams{};
  if (frame.shadowsEnabled) {
    shadowParams.cascadeCount = frame.cascadeCount;
    shadowParams.lightViewProj = frame.cascadeLightViewProj;
    shadowParams.splitDistances = frame.cascadeSplitDistances;
    const uint32_t smSize = m_shadow.Size();
    if (smSize > 0) {
      shadowParams.texelSize = m_shadow.TexelSize();
      shadowParams.bias = frame.shadowBias;
      shadowParams.strength = frame.shadowStrength;
      shadowParams.shadowSrvGpu = m_shadow.SrvGpu();
    }
  }

  // Draw opaque meshes (instanced batching — Phase 12.5).
  auto batches = BuildBatches(frame.opaqueItems);
  for (const auto &batch : batches) {
    m_mesh.DrawMeshInstanced(dx, batch.meshId, batch.worldMatrices,
                             frame.view, frame.proj, frame.lighting,
                             shadowParams, frame.gameTime);
  }
}

// ============================================================================
// GBufferPass — write surface properties to 4 MRT (Phase 12.1).
// ============================================================================
void GBufferPass::Execute(DxContext &dx, const FrameData &frame) {
  // Clear G-buffer render targets.
  const float albedoClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float normalClear[4] = {0.0f, 1.0f, 0.0f, 0.0f}; // up-facing default
  const float matClear[4] = {0.0f, 0.5f, 1.0f, 0.0f};    // default roughness 0.5, AO 1.0
  const float emisClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  dx.CmdList()->ClearRenderTargetView(dx.GBufferAlbedoRtv(), albedoClear, 0, nullptr);
  dx.CmdList()->ClearRenderTargetView(dx.GBufferNormalRtv(), normalClear, 0, nullptr);
  dx.CmdList()->ClearRenderTargetView(dx.GBufferMaterialRtv(), matClear, 0, nullptr);
  dx.CmdList()->ClearRenderTargetView(dx.GBufferEmissiveRtv(), emisClear, 0, nullptr);

  // Bind 4-target MRT + depth.
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
      dx.GBufferAlbedoRtv(), dx.GBufferNormalRtv(),
      dx.GBufferMaterialRtv(), dx.GBufferEmissiveRtv()};
  auto dsv = dx.Dsv();
  dx.CmdList()->OMSetRenderTargets(4, rtvs, FALSE, &dsv);

  // Draw opaque meshes into G-buffer (instanced batching — Phase 12.5).
  auto batches = BuildBatches(frame.opaqueItems);
  for (const auto &batch : batches) {
    m_mesh.DrawMeshGBufferInstanced(dx, batch.meshId, batch.worldMatrices,
                                    frame.view, frame.proj, frame.cameraPos,
                                    frame.gameTime);
  }
}

// ============================================================================
// DeferredLightingPass — fullscreen PBR lighting from G-buffer (Phase 12.1).
// ============================================================================
static Microsoft::WRL::ComPtr<ID3DBlob>
CompileShaderFromFile(const wchar_t *file, const char *entry,
                      const char *target) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                  entry, target, flags, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      OutputDebugStringA((const char *)errors->GetBufferPointer());
    ThrowIfFailed(hr, "Shader compile failed");
  }
  return code;
}

void DeferredLightingPass::CreatePipelineOnce(DxContext &dx) {
  if (m_pso && m_rootSig)
    return;

  // Root signature:
  // Param 0: root CBV b0 (LightingCB)
  // Param 1: SRV table t0-t4 (G-buffer: albedo, normal, material, emissive, depth)
  // Param 2: SRV table t5 (shadow map array)
  // Param 3: SRV table t6-t8 (IBL: irradiance, prefiltered, BRDF LUT)
  D3D12_DESCRIPTOR_RANGE gbufferRange{};
  gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  gbufferRange.NumDescriptors = 5;
  gbufferRange.BaseShaderRegister = 0; // t0-t4
  gbufferRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE shadowRange{};
  shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  shadowRange.NumDescriptors = 1;
  shadowRange.BaseShaderRegister = 5; // t5
  shadowRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE iblRange{};
  iblRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  iblRange.NumDescriptors = 3;
  iblRange.BaseShaderRegister = 6; // t6-t8
  iblRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[6]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &gbufferRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &shadowRange;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 1;
  params[3].DescriptorTable.pDescriptorRanges = &iblRange;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Param 4: root SRV t9 — StructuredBuffer<PointLight>
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[4].Descriptor.ShaderRegister = 9;
  params[4].Descriptor.RegisterSpace = 0;
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Param 5: root SRV t10 — StructuredBuffer<SpotLight>
  params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[5].Descriptor.ShaderRegister = 10;
  params[5].Descriptor.RegisterSpace = 0;
  params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samplers[3]{};
  // s0: point clamp for G-buffer sampling
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[0].ShaderRegister = 0;
  samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s1: shadow comparison sampler
  samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[1].ShaderRegister = 1;
  samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s2: IBL sampler (linear clamp)
  samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[2].ShaderRegister = 2;
  samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 6;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 3;
  rsDesc.pStaticSamplers = samplers;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "Serialize Deferred Lighting RS failed");
  ThrowIfFailed(dx.Device()->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                 rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rootSig)),
                "Create Deferred Lighting RS failed");

  auto vs = CompileShaderFromFile(L"shaders/deferred_lighting.hlsl",
                                  "VSFullscreen", "vs_5_1");
  auto ps = CompileShaderFromFile(L"shaders/deferred_lighting.hlsl",
                                  "PSMain", "ps_5_1");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_NONE;
  rast.DepthClipEnable = FALSE;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = FALSE;
  pso.DepthStencilState = ds;

  pso.InputLayout = {nullptr, 0};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = dx.HdrFormat();
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(dx.Device()->CreateGraphicsPipelineState(&pso,
                                                         IID_PPV_ARGS(&m_pso)),
                "Create Deferred Lighting PSO failed");
}

std::string DeferredLightingPass::ReloadShaders(DxContext &dx) {
  std::string errors;
  if (!m_rootSig) return errors;
  auto vs = CompileShaderSafe(L"shaders/deferred_lighting.hlsl", "VSFullscreen", "vs_5_1");
  auto ps = CompileShaderSafe(L"shaders/deferred_lighting.hlsl", "PSMain", "ps_5_1");
  if (vs.success && ps.success) {
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
    rast.DepthClipEnable = FALSE;
    pso.RasterizerState = rast;
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    pso.DepthStencilState = ds;
    pso.InputLayout = {nullptr, 0};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = dx.HdrFormat();
    pso.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> newPso;
    if (SUCCEEDED(dx.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
      m_pso = newPso;
  } else {
    if (!vs.success) errors += "[deferred_lighting.hlsl VS] " + vs.errorMessage + "\n";
    if (!ps.success) errors += "[deferred_lighting.hlsl PS] " + ps.errorMessage + "\n";
  }
  return errors;
}

void DeferredLightingPass::Execute(DxContext &dx, const FrameData &frame) {
  CreatePipelineOnce(dx);

  // Transition G-buffer targets: RT → SRV
  dx.Transition(dx.GBufferAlbedoTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferNormalTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferMaterialTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferEmissiveTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // Transition depth: DEPTH_WRITE → SRV
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  auto *cmd = dx.CmdList();

  // Build lighting constant buffer.
  struct LightingCB {
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT4X4 viewMat;
    DirectX::XMFLOAT4 cameraPos;
    DirectX::XMFLOAT4 lightDirIntensity;
    DirectX::XMFLOAT4 lightColor;
    DirectX::XMFLOAT4X4 cascadeLightViewProj[4];
    DirectX::XMFLOAT4 shadowParams;
    DirectX::XMFLOAT4 cascadeSplits;
    DirectX::XMFLOAT4 cascadeDebug;
    DirectX::XMFLOAT4 lightCounts; // x=numPointLights, y=numSpotLights
  };

  LightingCB cb{};
  DirectX::XMMATRIX vp = frame.view * frame.proj;
  DirectX::XMMATRIX invVP = DirectX::XMMatrixInverse(nullptr, vp);
  DirectX::XMStoreFloat4x4(&cb.invViewProj, DirectX::XMMatrixTranspose(invVP));
  DirectX::XMStoreFloat4x4(&cb.viewMat, DirectX::XMMatrixTranspose(frame.view));
  cb.cameraPos = {frame.cameraPos.x, frame.cameraPos.y, frame.cameraPos.z, 0.0f};
  cb.lightDirIntensity = {frame.lighting.lightDir.x, frame.lighting.lightDir.y,
                          frame.lighting.lightDir.z, frame.lighting.lightIntensity};
  cb.lightColor = {frame.lighting.lightColor.x, frame.lighting.lightColor.y,
                   frame.lighting.lightColor.z, frame.lighting.iblIntensity};

  for (uint32_t c = 0; c < frame.cascadeCount && c < kMaxCascades; ++c)
    DirectX::XMStoreFloat4x4(&cb.cascadeLightViewProj[c],
                              DirectX::XMMatrixTranspose(frame.cascadeLightViewProj[c]));

  if (frame.shadowsEnabled && m_shadow.Size() > 0) {
    const auto ts = m_shadow.TexelSize();
    cb.shadowParams = {ts.x, ts.y, frame.shadowBias, frame.shadowStrength};
    cb.cascadeSplits = {frame.cascadeSplitDistances[0],
                        frame.cascadeSplitDistances[1],
                        frame.cascadeSplitDistances[2],
                        static_cast<float>(frame.cascadeCount)};
  }
  cb.cascadeDebug = {frame.lighting.cascadeDebug, 0.0f, 0.0f, 0.0f};
  cb.lightCounts = {static_cast<float>(frame.pointLights.size()),
                    static_cast<float>(frame.spotLights.size()),
                    0.0f, 0.0f};

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(LightingCB), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(LightingCB));

  // Upload point lights (always allocate at least 1 element for valid GPU VA).
  uint32_t plBytes = static_cast<uint32_t>(
      std::max(frame.pointLights.size(), size_t(1)) * sizeof(GPUPointLight));
  void *plCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS plGpu = dx.AllocFrameConstants(plBytes, &plCpu);
  if (!frame.pointLights.empty())
    memcpy(plCpu, frame.pointLights.data(),
           frame.pointLights.size() * sizeof(GPUPointLight));

  // Upload spot lights.
  uint32_t slBytes = static_cast<uint32_t>(
      std::max(frame.spotLights.size(), size_t(1)) * sizeof(GPUSpotLight));
  void *slCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS slGpu = dx.AllocFrameConstants(slBytes, &slCpu);
  if (!frame.spotLights.empty())
    memcpy(slCpu, frame.spotLights.data(),
           frame.spotLights.size() * sizeof(GPUSpotLight));

  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_rootSig.Get());
  cmd->SetPipelineState(m_pso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Bind root parameters.
  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  cmd->SetGraphicsRootDescriptorTable(1, dx.GBufferTableSrvGpu()); // t0-t4
  if (frame.shadowsEnabled && m_shadow.Size() > 0)
    cmd->SetGraphicsRootDescriptorTable(2, m_shadow.SrvGpu());     // t5
  auto iblGpu = dx.IblTableGpu();
  if (iblGpu.ptr != 0)
    cmd->SetGraphicsRootDescriptorTable(3, iblGpu);                // t6-t8
  cmd->SetGraphicsRootShaderResourceView(4, plGpu);                // t9
  cmd->SetGraphicsRootShaderResourceView(5, slGpu);                // t10

  // Draw fullscreen triangle into HDR target.
  auto hdrRtv = dx.HdrRtv();
  cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
  dx.SetViewportScissorFull();
  cmd->DrawInstanced(3, 1, 0, 0);

  // Transition depth back: SRV → DEPTH_WRITE
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
  // G-buffer stays in SRV state (SSAO reads normals from it).
}

// ============================================================================
// GridPass — draw grid/axes into HDR (after deferred lighting).
// ============================================================================
void GridPass::Execute(DxContext &dx, const FrameData &frame) {
  // Bind HDR + depth for forward grid rendering.
  auto hdrRtv = dx.HdrRtv();
  auto dsv = dx.Dsv();
  dx.CmdList()->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);
  m_grid.Draw(dx, frame.view, frame.proj);
}

// ============================================================================
// SSAOPass — screen-space ambient occlusion (generation + bilateral blur).
// ============================================================================
void SSAOPass::Execute(DxContext &dx, const FrameData &frame) {
  // Always transition view normals RT → SRV so BeginFrame's SRV → RT works
  // next frame, even when SSAO is disabled.
  dx.Transition(dx.ViewNormalTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  if (!frame.ssaoEnabled)
    return;

  // Transition depth: DEPTH_WRITE → PIXEL_SHADER_RESOURCE
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // G-buffer normals already in SRV state (from DeferredLightingPass).
  // Transition SSAO target: SRV → RENDER_TARGET
  dx.Transition(dx.SsaoTarget(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

  m_ssao.ExecuteSSAO(dx, frame.proj, frame.view, frame.ssaoRadius,
                     frame.ssaoBias, frame.ssaoPower, frame.ssaoKernelSize);

  // Transition SSAO target: RT → SRV, blur target: SRV → RT
  dx.Transition(dx.SsaoTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.SsaoBlurTarget(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

  m_ssao.ExecuteBlur(dx);

  // Transition blur target: RT → SRV (ready for tonemap read)
  dx.Transition(dx.SsaoBlurTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition depth back: SRV → DEPTH_WRITE
  dx.Transition(dx.DepthBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ============================================================================
// TransparentPass — particles with additive blending.
// ============================================================================
void TransparentPass::Execute(DxContext &dx, const FrameData &frame) {
  if (!frame.particlesEnabled || frame.emitters.empty())
    return;

  m_particles.DrawParticles(dx, frame.emitters, frame.view, frame.proj);
}

// ============================================================================
// BloomPass — HDR downsample + upsample bloom.
// ============================================================================
void BloomPass::Execute(DxContext &dx, const FrameData &frame) {
  // When TAA is active, TAAPass already transitioned HDR RT->SRV.
  // Otherwise we do it here.
  if (!frame.taaEnabled) {
    dx.Transition(dx.HdrTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  PostProcessParams params;
  params.exposure = frame.exposure;
  params.bloomThreshold = frame.bloomThreshold;
  params.bloomIntensity = frame.bloomIntensity;
  params.bloomEnabled = frame.bloomEnabled;
  params.fxaaEnabled = frame.fxaaEnabled;

  // When TAA is active, bloom reads TAA output instead of raw HDR.
  if (frame.taaEnabled) {
    params.hdrOverrideSrvGpu = dx.TaaOutputSrvGpu();
  }

  m_pp.ExecuteBloom(dx, params);
}

// ============================================================================
// TonemapPass — HDR + bloom composite -> LDR (ACES filmic).
// ============================================================================
void TonemapPass::Execute(DxContext &dx, const FrameData &frame) {
  PostProcessParams params;
  params.exposure = frame.exposure;
  params.bloomThreshold = frame.bloomThreshold;
  params.bloomIntensity = frame.bloomIntensity;
  params.bloomEnabled = frame.bloomEnabled;
  params.fxaaEnabled = frame.fxaaEnabled;
  params.motionBlurEnabled = frame.motionBlurEnabled && frame.hasPrevViewProj;
  params.dofEnabled = frame.dofEnabled;
  params.taaEnabled = frame.taaEnabled;
  params.aoStrength = frame.ssaoEnabled ? frame.ssaoStrength : 0.0f;
  params.aoSrvGpu = dx.SsaoBlurSrvGpu();

  // When TAA is active, tonemap reads TAA output instead of raw HDR.
  if (frame.taaEnabled) {
    params.hdrOverrideSrvGpu = dx.TaaOutputSrvGpu();
  }

  m_pp.ExecuteTonemap(dx, params);
}

// ============================================================================
// FXAAPass — Screen-space anti-aliasing.
// ============================================================================
void FXAAPass::Execute(DxContext &dx, const FrameData &frame) {
  PostProcessParams params;
  params.fxaaEnabled = frame.fxaaEnabled;
  params.motionBlurEnabled = frame.motionBlurEnabled && frame.hasPrevViewProj;
  params.dofEnabled = frame.dofEnabled;

  m_pp.ExecuteFXAA(dx, params);
}

// ============================================================================
// DOFPass — Depth of Field (Phase 10.6).
// ============================================================================
void DOFPass::Execute(DxContext &dx, const FrameData &frame) {
  if (!frame.dofEnabled)
    return;

  // LDR is already SRV (from tonemap).
  // Transition depth: DEPTH_WRITE -> SRV
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition DOF target: SRV -> RT
  dx.Transition(dx.DofTarget(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

  PostProcessParams params;
  params.dofEnabled = frame.dofEnabled;
  params.dofFocalDistance = frame.dofFocalDistance;
  params.dofFocalRange = frame.dofFocalRange;
  params.dofMaxBlur = frame.dofMaxBlur;
  params.nearZ = 0.1f;
  params.farZ = 1000.0f;

  m_pp.ExecuteDOF(dx, params);

  // Transition DOF target: RT -> SRV (for FXAA or motion blur to read)
  dx.Transition(dx.DofTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition depth back: SRV -> DEPTH_WRITE
  dx.Transition(dx.DepthBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ============================================================================
// VelocityGenPass — generate per-pixel screen-space velocity from depth.
// ============================================================================
void VelocityGenPass::Execute(DxContext &dx, const FrameData &frame) {
  // Run velocity gen for TAA or motion blur (either needs it).
  const bool needVelocity =
      (frame.taaEnabled || frame.motionBlurEnabled) && frame.hasPrevViewProj;
  if (!needVelocity)
    return;

  // Transition depth: DEPTH_WRITE -> SRV
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition velocity target: SRV -> RT
  dx.Transition(dx.VelocityTarget(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Use unjittered matrices when TAA is active so velocity reflects
  // actual camera/object motion, not jitter noise.
  if (frame.taaEnabled) {
    m_pp.ExecuteVelocityGen(dx, frame.invViewProjUnjittered,
                            frame.prevViewProjUnjittered);
  } else {
    m_pp.ExecuteVelocityGen(dx, frame.invViewProj, frame.prevViewProj);
  }

  // Transition velocity: RT -> SRV
  dx.Transition(dx.VelocityTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition depth back: SRV -> DEPTH_WRITE
  dx.Transition(dx.DepthBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ============================================================================
// TAAPass — temporal anti-aliasing resolve (Phase 10.4).
// ============================================================================
void TAAPass::Execute(DxContext &dx, const FrameData &frame) {
  if (!frame.taaEnabled)
    return;

  // Transition HDR: RT -> SRV (TAA reads current frame).
  dx.Transition(dx.HdrTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Transition TAA output buffer: SRV -> RT.
  dx.Transition(dx.TaaOutputTarget(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
  // History buffer stays in SRV state for reading.

  PostProcessParams params;
  params.taaEnabled = frame.taaEnabled;
  params.taaBlendFactor = frame.taaBlendFactor;
  m_pp.ExecuteTAA(dx, params);

  // Transition TAA output: RT -> SRV (bloom/tonemap will read this).
  dx.Transition(dx.TaaOutputTarget(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  dx.ClearTaaFirstFrame();
  // NOTE: Do NOT swap TAA buffers here — bloom/tonemap still need to read
  // the output we just wrote (TaaOutputSrvGpu). Swap happens at end of frame.
}

// ============================================================================
// MotionBlurPass — directional blur along velocity vectors.
// ============================================================================
void MotionBlurPass::Execute(DxContext &dx, const FrameData &frame) {
  if (!frame.motionBlurEnabled || !frame.hasPrevViewProj)
    return;

  // LDR (or DOF target) is already SRV. Velocity is already SRV.
  // Transition LDR2: SRV -> RT
  dx.Transition(dx.Ldr2Target(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

  PostProcessParams params;
  params.motionBlurEnabled = frame.motionBlurEnabled;
  params.motionBlurStrength = frame.motionBlurStrength;
  params.motionBlurSamples = frame.motionBlurSamples;
  params.dofEnabled = frame.dofEnabled;

  m_pp.ExecuteMotionBlur(dx, params);

  // Transition LDR2: RT -> SRV (for FXAA to read)
  dx.Transition(dx.Ldr2Target(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ============================================================================
// HighlightPass — wireframe overlay for selected entities (editor).
// ============================================================================
void HighlightPass::Execute(DxContext &dx, const FrameData &frame) {
  if (frame.highlightItems.empty())
    return;

  // Render into the HDR target with depth test (no depth write).
  auto hdrRtv = dx.HdrRtv();
  auto dsv = dx.Dsv();
  dx.CmdList()->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);

  const DirectX::XMFLOAT4 highlightColor = {1.0f, 0.9f, 0.1f, 1.0f}; // yellow
  for (const auto &item : frame.highlightItems) {
    m_mesh.DrawMeshWireframe(dx, item.meshId, item.world, frame.view,
                             frame.proj, highlightColor);
  }
}

// ============================================================================
// UIPass — Dear ImGui overlay (renders to backbuffer).
// ============================================================================
void UIPass::Execute(DxContext &dx, const FrameData & /*frame*/) {
  // Ensure we're rendering to the backbuffer for UI overlay.
  auto rtv = dx.CurrentRtv();
  dx.CmdList()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  dx.SetViewportScissorFull();
  m_imgui.Render(dx);
}
