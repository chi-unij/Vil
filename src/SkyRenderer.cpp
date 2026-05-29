// ======================================
// File: SkyRenderer.cpp
// Purpose: HDRI sky background renderer implementation (extracted from
//          DxContext::CreateSkyResources + DxContext::DrawSky, Phase 8)
// ======================================

#include "SkyRenderer.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "HdriLoader.h"
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

static UINT Align256(UINT size) { return (size + 255u) & ~255u; }

// GPU constant buffer layout for sky.hlsl.
struct SkyCB {
  DirectX::XMFLOAT4X4 invViewProj;
  DirectX::XMFLOAT3 cameraPos;
  float exposure;
};

void SkyRenderer::Initialize(DxContext &dx) {
  // ---- Root signature: CBV table (b0) + SRV table (t0) + static sampler ----
  D3D12_DESCRIPTOR_RANGE cbvRange{};
  cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  cbvRange.NumDescriptors = 1;
  cbvRange.BaseShaderRegister = 0; // b0
  cbvRange.RegisterSpace = 0;
  cbvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE srvRange{};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srvRange.NumDescriptors = 1;
  srvRange.BaseShaderRegister = 0; // t0
  srvRange.RegisterSpace = 0;
  srvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &srvRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  samp.ShaderRegister = 0; // s0
  samp.RegisterSpace = 0;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &samp;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob;
  ComPtr<ID3DBlob> rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(
                    &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError),
                "D3D12SerializeRootSignature (sky) failed");
  ThrowIfFailed(dx.m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                 rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rootSig)),
                "CreateRootSignature (sky) failed");

  // ---- PSO ----
  auto vs = CompileShaderLocal(L"shaders/sky.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderLocal(L"shaders/sky.hlsl", "PSMain", "ps_5_0");

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
  rast.DepthClipEnable = TRUE;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = FALSE;
  ds.StencilEnable = FALSE;
  pso.DepthStencilState = ds;

  pso.InputLayout = {nullptr, 0};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = dx.m_hdrFormat;
  pso.SampleDesc.Count = 1;

  ThrowIfFailed(
      dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)),
      "CreateGraphicsPipelineState (sky) failed");

  // ---- Load HDRI EXR ----
  const HdriImageRgba32f img =
      LoadExrRgba32f(L"Assets/HDRI/citrus_orchard_road_puresky_2k.exr");
  if (img.width <= 0 || img.height <= 0 || img.rgba.empty())
    throw std::runtime_error("SkyRenderer::Initialize: EXR image is empty");

  // ---- GPU texture (float32 RGBA) ----
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = static_cast<UINT64>(img.width);
  tex.Height = static_cast<UINT>(img.height);
  tex.DepthOrArraySize = 1;
  tex.MipLevels = 1;
  tex.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  tex.SampleDesc.Count = 1;
  tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  tex.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ThrowIfFailed(
      dx.m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                           &tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, IID_PPV_ARGS(&m_tex)),
      "CreateCommittedResource (SkyTex) failed");

  // ---- Upload buffer ----
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT numRows = 0;
  UINT64 rowSizeInBytes = 0;
  UINT64 totalBytes = 0;
  dx.m_device->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, &numRows,
                                     &rowSizeInBytes, &totalBytes);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC uploadDesc{};
  uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uploadDesc.Width = totalBytes;
  uploadDesc.Height = 1;
  uploadDesc.DepthOrArraySize = 1;
  uploadDesc.MipLevels = 1;
  uploadDesc.SampleDesc.Count = 1;
  uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(dx.m_device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&m_texUpload)),
                "CreateCommittedResource (SkyTexUpload) failed");

  // ---- Copy CPU pixels -> upload buffer ----
  uint8_t *uploadMapped = nullptr;
  D3D12_RANGE mapRange{0, 0};
  ThrowIfFailed(m_texUpload->Map(0, &mapRange,
                                 reinterpret_cast<void **>(&uploadMapped)),
                "SkyTexUpload Map failed");
  const uint8_t *src = reinterpret_cast<const uint8_t *>(img.rgba.data());
  const UINT srcRowBytes =
      static_cast<UINT>(img.width) * 16u; // RGBA32F = 16 bytes/pixel
  for (UINT y = 0; y < static_cast<UINT>(img.height); ++y) {
    uint8_t *dstRow = uploadMapped + footprint.Offset +
                      static_cast<UINT64>(y) * footprint.Footprint.RowPitch;
    const uint8_t *srcRow = src + static_cast<UINT64>(y) * srcRowBytes;
    memcpy(dstRow, srcRow, srcRowBytes);
  }
  m_texUpload->Unmap(0, nullptr);

  // ---- One-time GPU copy ----
  ThrowIfFailed(dx.m_frames[0].cmdAlloc->Reset(),
                "Sky upload allocator Reset failed");
  ThrowIfFailed(dx.m_cmdList->Reset(dx.m_frames[0].cmdAlloc.Get(), nullptr),
                "Sky upload cmdlist Reset failed");

  D3D12_TEXTURE_COPY_LOCATION dstLoc{};
  dstLoc.pResource = m_tex.Get();
  dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION srcLoc{};
  srcLoc.pResource = m_texUpload.Get();
  srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcLoc.PlacedFootprint = footprint;

  dx.m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

  dx.Transition(m_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  ThrowIfFailed(dx.m_cmdList->Close(), "Sky upload cmdlist Close failed");
  ID3D12CommandList *lists[] = {dx.m_cmdList.Get()};
  dx.m_queue->ExecuteCommandLists(1, lists);
  dx.WaitForGpu();

  // ---- SRV in main heap ----
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Format = tex.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;

  D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = dx.AllocMainSrvCpu(1);
  dx.m_device->CreateShaderResourceView(m_tex.Get(), &srv, srvCpu);
  m_srvGpu = dx.MainSrvGpuFromCpu(srvCpu);

  // ---- Per-frame sky constant buffers ----
  const UINT cbSize = Align256(sizeof(SkyCB));

  D3D12_RESOURCE_DESC cbDesc{};
  cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  cbDesc.Width = cbSize;
  cbDesc.Height = 1;
  cbDesc.DepthOrArraySize = 1;
  cbDesc.MipLevels = 1;
  cbDesc.SampleDesc.Count = 1;
  cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  for (uint32_t i = 0; i < kFrameCount; ++i) {
    ThrowIfFailed(dx.m_device->CreateCommittedResource(
                      &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                      IID_PPV_ARGS(&m_frameCBs[i].cb)),
                  "CreateCommittedResource (SkyCB per-frame) failed");

    ThrowIfFailed(m_frameCBs[i].cb->Map(
                      0, &mapRange,
                      reinterpret_cast<void **>(&m_frameCBs[i].mapped)),
                  "SkyCB Map failed");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = m_frameCBs[i].cb->GetGPUVirtualAddress();
    cbv.SizeInBytes = cbSize;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = dx.AllocMainSrvCpu(1);
    dx.m_device->CreateConstantBufferView(&cbv, cpu);
    m_frameCBs[i].gpuHandle = dx.MainSrvGpuFromCpu(cpu);
  }
}

void SkyRenderer::Draw(DxContext &dx, const DirectX::XMMATRIX &view,
                        const DirectX::XMMATRIX &proj, float exposure) {
  using namespace DirectX;

  // Inverse of (view * proj), for ray reconstruction.
  XMMATRIX vp = view * proj;
  XMMATRIX invVp = XMMatrixInverse(nullptr, vp);

  // Extract camera position from inverse view matrix.
  XMMATRIX invView = XMMatrixInverse(nullptr, view);
  XMFLOAT3 camPos{};
  XMStoreFloat3(&camPos, invView.r[3]);

  SkyCB cb{};
  XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVp));
  cb.cameraPos = camPos;
  cb.exposure = exposure;

  const uint32_t fi = dx.FrameIndex();
  memcpy(m_frameCBs[fi].mapped, &cb, sizeof(cb));

  dx.SetViewportScissorFull();

  auto rtv = dx.HdrRtv();
  auto dsv = dx.Dsv();
  dx.m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

  dx.m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());
  dx.m_cmdList->SetPipelineState(m_pso.Get());

  ID3D12DescriptorHeap *heaps[] = {dx.m_mainSrvHeap.Get()};
  dx.m_cmdList->SetDescriptorHeaps(1, heaps);

  // Root param 0 = CBV table (b0), root param 1 = SRV table (t0).
  dx.m_cmdList->SetGraphicsRootDescriptorTable(0, m_frameCBs[fi].gpuHandle);
  dx.m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvGpu);

  dx.m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dx.m_cmdList->DrawInstanced(3, 1, 0, 0);
}

std::string SkyRenderer::ReloadShaders(DxContext &dx) {
  std::string errors;
  if (!m_rootSig) return errors;
  auto vs = CompileShaderSafe(L"shaders/sky.hlsl", "VSMain", "vs_5_0");
  auto ps = CompileShaderSafe(L"shaders/sky.hlsl", "PSMain", "ps_5_0");
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
    rast.DepthClipEnable = TRUE;
    pso.RasterizerState = rast;
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    pso.DepthStencilState = ds;
    pso.InputLayout = {nullptr, 0};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = dx.m_hdrFormat;
    pso.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> newPso;
    if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
      m_pso = newPso;
  } else {
    if (!vs.success) errors += "[sky.hlsl VS] " + vs.errorMessage + "\n";
    if (!ps.success) errors += "[sky.hlsl PS] " + ps.errorMessage + "\n";
  }
  return errors;
}

void SkyRenderer::Reset() {
  for (auto &fcb : m_frameCBs) {
    if (fcb.cb && fcb.mapped) {
      fcb.cb->Unmap(0, nullptr);
      fcb.mapped = nullptr;
    }
    fcb.cb.Reset();
    fcb.gpuHandle = {};
  }
  m_pso.Reset();
  m_rootSig.Reset();
  m_tex.Reset();
  m_texUpload.Reset();
  m_srvGpu = {};
}
