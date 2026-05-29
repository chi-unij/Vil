// ======================================
// File: IBLGenerator.cpp
// Purpose: IBL precomputation implementation — equirect->cubemap, irradiance,
//          prefiltered specular, and BRDF LUT generation on the GPU.
// ======================================

#include "IBLGenerator.h"
#include "DxContext.h"
#include "DxUtil.h"

#include <DirectXMath.h>
#include <cstring>
#include <d3dcompiler.h>
#include <stdexcept>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---- Helper: compile shader from file ----
static ComPtr<ID3DBlob> CompileShader(const wchar_t *filePath,
                                      const char *entryPoint,
                                      const char *target) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

  ComPtr<ID3DBlob> bytecode, errors;
  HRESULT hr =
      D3DCompileFromFile(filePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                         entryPoint, target, flags, 0, &bytecode, &errors);
  if (FAILED(hr)) {
    if (errors) {
      std::string msg((const char *)errors->GetBufferPointer(),
                      errors->GetBufferSize());
      throw std::runtime_error("IBL shader compile: " + msg);
    }
    ThrowIfFailed(hr, "IBL D3DCompileFromFile failed");
  }
  return bytecode;
}

// ---- Helper: create a cubemap (2D array with 6 slices) ----
static ComPtr<ID3D12Resource> CreateCubemap(ID3D12Device *dev, uint32_t size,
                                            uint32_t mipLevels,
                                            DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = size;
  desc.Height = size;
  desc.DepthOrArraySize = 6;
  desc.MipLevels = static_cast<UINT16>(mipLevels);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = format;

  ComPtr<ID3D12Resource> res;
  ThrowIfFailed(
      dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                   IID_PPV_ARGS(&res)),
      "CreateCubemap failed");
  return res;
}

// ---- Helper: create a 2D texture ----
static ComPtr<ID3D12Resource> CreateTexture2D(ID3D12Device *dev, uint32_t w,
                                              uint32_t h, DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = format;

  ComPtr<ID3D12Resource> res;
  ThrowIfFailed(
      dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                   IID_PPV_ARGS(&res)),
      "CreateTexture2D failed");
  return res;
}

// ---- Helper: create cubemap SRV ----
static D3D12_GPU_DESCRIPTOR_HANDLE
CreateCubeSrv(DxContext &dx, ID3D12Resource *res, DXGI_FORMAT format,
              uint32_t mipLevels) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  srv.TextureCube.MipLevels = mipLevels;
  srv.TextureCube.MostDetailedMip = 0;

  auto cpu = dx.AllocMainSrvCpu(1);
  dx.Device()->CreateShaderResourceView(res, &srv, cpu);
  return dx.MainSrvGpuFromCpu(cpu);
}

// ---- Helper: create 2D SRV ----
static D3D12_GPU_DESCRIPTOR_HANDLE CreateTex2DSrv(DxContext &dx,
                                                   ID3D12Resource *res,
                                                   DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;

  auto cpu = dx.AllocMainSrvCpu(1);
  dx.Device()->CreateShaderResourceView(res, &srv, cpu);
  return dx.MainSrvGpuFromCpu(cpu);
}

// ======================================
// IBLGenerator public methods
// ======================================

void IBLGenerator::Initialize(DxContext &dx, ID3D12Resource * /*equirectHdri*/,
                              D3D12_GPU_DESCRIPTOR_HANDLE equirectSrvGpu) {
  CreateResources(dx);
  CreatePrecomputePipelines(dx);
  RunPrecomputation(dx, equirectSrvGpu);

  // Release temporary pipelines and RTV heap (no longer needed).
  m_precomputeRootSig.Reset();
  m_equirectToCubePso.Reset();
  m_irradiancePso.Reset();
  m_prefilteredPso.Reset();
  m_brdfLutPso.Reset();
  m_precomputeRtvHeap.Reset();
}

void IBLGenerator::Reset() {
  m_envCubemap.Reset();
  m_irradianceCubemap.Reset();
  m_prefilteredCubemap.Reset();
  m_brdfLut.Reset();
  m_precomputeRootSig.Reset();
  m_equirectToCubePso.Reset();
  m_irradiancePso.Reset();
  m_prefilteredPso.Reset();
  m_brdfLutPso.Reset();
  m_precomputeRtvHeap.Reset();
}

// ======================================
// Resource creation
// ======================================

void IBLGenerator::CreateResources(DxContext &dx) {
  auto dev = dx.Device();

  // GPU textures.
  m_envCubemap = CreateCubemap(dev, kEnvCubeSize, 1, kCubeFormat);
  m_irradianceCubemap = CreateCubemap(dev, kIrradianceSize, 1, kCubeFormat);
  m_prefilteredCubemap =
      CreateCubemap(dev, kPrefilteredSize, kPrefilteredMips, kCubeFormat);
  m_brdfLut = CreateTexture2D(dev, kBrdfLutSize, kBrdfLutSize, kBrdfFormat);

  // SRVs: env cubemap gets its own slot, then irradiance+prefiltered+brdfLut
  // are contiguous (for single descriptor table binding at t2,t3,t4).
  m_envCubeSrvGpu = CreateCubeSrv(dx, m_envCubemap.Get(), kCubeFormat, 1);

  // Contiguous block of 3 SRVs — allocate together.
  auto irrCpu = dx.AllocMainSrvCpu(3);
  m_iblTableGpuBase = dx.MainSrvGpuFromCpu(irrCpu);

  // Irradiance SRV (slot 0 of block).
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kCubeFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = 1;
    dev->CreateShaderResourceView(m_irradianceCubemap.Get(), &srv, irrCpu);
  }

  // Prefiltered SRV (slot 1 of block).
  {
    D3D12_CPU_DESCRIPTOR_HANDLE prefCpu = irrCpu;
    prefCpu.ptr += dx.m_mainSrvDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kCubeFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = kPrefilteredMips;
    dev->CreateShaderResourceView(m_prefilteredCubemap.Get(), &srv, prefCpu);
  }

  // BRDF LUT SRV (slot 2 of block).
  {
    D3D12_CPU_DESCRIPTOR_HANDLE brdfCpu = irrCpu;
    brdfCpu.ptr += 2 * dx.m_mainSrvDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kBrdfFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(m_brdfLut.Get(), &srv, brdfCpu);
  }

  // Temporary RTV heap for precomputation.
  // 6 (env) + 6 (irradiance) + 30 (prefiltered: 6 faces × 5 mips) + 1 (BRDF
  // LUT) = 43.
  {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 43;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(dev->CreateDescriptorHeap(&rtvHeapDesc,
                                            IID_PPV_ARGS(&m_precomputeRtvHeap)),
                  "Create IBL RTV heap failed");
    m_rtvDescriptorSize =
        dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }

  // Create all RTVs.
  auto rtvBase = m_precomputeRtvHeap->GetCPUDescriptorHandleForHeapStart();
  uint32_t rtvIdx = 0;

  auto makeRtv = [&](ID3D12Resource *res, uint32_t face, uint32_t mip,
                     DXGI_FORMAT fmt) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = fmt;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv.Texture2DArray.MipSlice = mip;
    rtv.Texture2DArray.FirstArraySlice = face;
    rtv.Texture2DArray.ArraySize = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
    h.ptr += static_cast<SIZE_T>(rtvIdx) * m_rtvDescriptorSize;
    dev->CreateRenderTargetView(res, &rtv, h);
    rtvIdx++;
    return h;
  };

  // Env cubemap RTVs: index 0..5
  for (uint32_t f = 0; f < 6; ++f)
    makeRtv(m_envCubemap.Get(), f, 0, kCubeFormat);

  // Irradiance cubemap RTVs: index 6..11
  for (uint32_t f = 0; f < 6; ++f)
    makeRtv(m_irradianceCubemap.Get(), f, 0, kCubeFormat);

  // Prefiltered cubemap RTVs: index 12..41 (5 mips × 6 faces)
  for (uint32_t m = 0; m < kPrefilteredMips; ++m)
    for (uint32_t f = 0; f < 6; ++f)
      makeRtv(m_prefilteredCubemap.Get(), f, m, kCubeFormat);

  // BRDF LUT RTV: index 42
  {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kBrdfFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv.Texture2D.MipSlice = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
    h.ptr += static_cast<SIZE_T>(rtvIdx) * m_rtvDescriptorSize;
    dev->CreateRenderTargetView(m_brdfLut.Get(), &rtv, h);
  }
}

// ======================================
// Pipeline creation (4 PSOs, 1 root sig)
// ======================================

void IBLGenerator::CreatePrecomputePipelines(DxContext &dx) {
  auto dev = dx.Device();

  // Root signature: root CBV b0 + SRV table t0 + static sampler s0.
  D3D12_DESCRIPTOR_RANGE srvRange{};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srvRange.NumDescriptors = 1;
  srvRange.BaseShaderRegister = 0; // t0
  srvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0; // b0
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &srvRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0; // s0
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;

  ComPtr<ID3DBlob> rsBlob, rsError;
  ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &rsBlob, &rsError),
                "Serialize IBL RS failed");
  ThrowIfFailed(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                         rsBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&m_precomputeRootSig)),
                "Create IBL RS failed");

  // Compile shaders.
  auto vs = CompileShader(L"shaders/ibl.hlsl", "VSMain", "vs_5_0");
  auto psEquirect =
      CompileShader(L"shaders/ibl.hlsl", "EquirectToCubePS", "ps_5_0");
  auto psIrradiance =
      CompileShader(L"shaders/ibl.hlsl", "IrradiancePS", "ps_5_0");
  auto psPrefiltered =
      CompileShader(L"shaders/ibl.hlsl", "PrefilteredPS", "ps_5_0");
  auto psBrdfLut = CompileShader(L"shaders/ibl.hlsl", "BrdfLutPS", "ps_5_0");

  // Helper lambda to build PSO.
  auto makePso = [&](ID3DBlob *ps, DXGI_FORMAT rtvFormat) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_precomputeRootSig.Get();
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

    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat;
    pso.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> result;
    ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&result)),
        "Create IBL PSO failed");
    return result;
  };

  m_equirectToCubePso = makePso(psEquirect.Get(), kCubeFormat);
  m_irradiancePso = makePso(psIrradiance.Get(), kCubeFormat);
  m_prefilteredPso = makePso(psPrefiltered.Get(), kCubeFormat);
  m_brdfLutPso = makePso(psBrdfLut.Get(), kBrdfFormat);
}

// ======================================
// GPU precomputation
// ======================================

void IBLGenerator::RunPrecomputation(
    DxContext &dx, D3D12_GPU_DESCRIPTOR_HANDLE equirectSrvGpu) {
  auto dev = dx.Device();
  auto cmd = dx.CmdList();

  // Reset command list for init-time work.
  auto &alloc = dx.m_frames[0].cmdAlloc;
  ThrowIfFailed(alloc->Reset(), "IBL CmdAlloc Reset");
  ThrowIfFailed(cmd->Reset(alloc.Get(), nullptr), "IBL CmdList Reset");

  // Constant buffer for per-face data (upload heap, single allocation).
  // IBLConvolveCB: float4x4 (64) + float (4) + float (4) + float2 (8) = 80
  // bytes. Aligned to 256.
  struct IBLConvolveCB {
    XMFLOAT4X4 faceInvViewProj;
    float roughness;
    float envCubeSize;
    float pad[2];
  };
  static_assert(sizeof(IBLConvolveCB) <= 256, "CB must fit in 256 bytes");

  // We need CBs for: 6 (equirect) + 6 (irradiance) + 30 (prefiltered) + 1
  // (BRDF) = 43. Use a single upload buffer.
  const uint32_t cbAlignedSize = 256;
  const uint32_t totalCBs = 43;
  const uint32_t uploadSize = cbAlignedSize * totalCBs;

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufDesc{};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = uploadSize;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> cbUpload;
  ThrowIfFailed(dev->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&cbUpload)),
                "IBL CB upload buffer");

  uint8_t *cbMapped = nullptr;
  cbUpload->Map(0, nullptr, reinterpret_cast<void **>(&cbMapped));

  // Face view matrices (left-handed, looking from origin).
  const XMVECTOR origin = XMVectorZero();
  const XMMATRIX faceViews[6] = {
      XMMatrixLookAtLH(origin, XMVectorSet(+1, 0, 0, 0),
                       XMVectorSet(0, +1, 0, 0)), // +X
      XMMatrixLookAtLH(origin, XMVectorSet(-1, 0, 0, 0),
                       XMVectorSet(0, +1, 0, 0)), // -X
      XMMatrixLookAtLH(origin, XMVectorSet(0, +1, 0, 0),
                       XMVectorSet(0, 0, -1, 0)), // +Y
      XMMatrixLookAtLH(origin, XMVectorSet(0, -1, 0, 0),
                       XMVectorSet(0, 0, +1, 0)), // -Y
      XMMatrixLookAtLH(origin, XMVectorSet(0, 0, +1, 0),
                       XMVectorSet(0, +1, 0, 0)), // +Z
      XMMatrixLookAtLH(origin, XMVectorSet(0, 0, -1, 0),
                       XMVectorSet(0, +1, 0, 0)), // -Z
  };
  const XMMATRIX proj =
      XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 10.0f);

  // Helper: fill a CB at a given index.
  auto fillCB = [&](uint32_t idx, uint32_t faceIdx, float roughness,
                    float envSize) {
    IBLConvolveCB cb{};
    XMMATRIX vp = faceViews[faceIdx] * proj;
    XMMATRIX invVP = XMMatrixInverse(nullptr, vp);
    XMStoreFloat4x4(&cb.faceInvViewProj, XMMatrixTranspose(invVP));
    cb.roughness = roughness;
    cb.envCubeSize = envSize;
    memcpy(cbMapped + idx * cbAlignedSize, &cb, sizeof(IBLConvolveCB));
  };

  // Helper to get an RTV handle by index.
  auto rtvBase = m_precomputeRtvHeap->GetCPUDescriptorHandleForHeapStart();
  auto getRtv = [&](uint32_t idx) -> D3D12_CPU_DESCRIPTOR_HANDLE {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
    h.ptr += static_cast<SIZE_T>(idx) * m_rtvDescriptorSize;
    return h;
  };

  // Helper to set viewport + scissor.
  auto setViewport = [&](uint32_t size) {
    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(size);
    vp.Height = static_cast<float>(size);
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(size),
                       static_cast<LONG>(size)};
    cmd->RSSetScissorRects(1, &scissor);
  };

  // Bind the main SRV heap (needed for texture binding).
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);

  uint32_t cbIdx = 0;

  // ---- Pass 1: Equirectangular -> Cubemap ----
  cmd->SetPipelineState(m_equirectToCubePso.Get());
  cmd->SetGraphicsRootSignature(m_precomputeRootSig.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  for (uint32_t f = 0; f < 6; ++f) {
    fillCB(cbIdx, f, 0.0f, 0.0f);

    auto rtv = getRtv(f); // env cubemap face f
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    setViewport(kEnvCubeSize);

    D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
        cbUpload->GetGPUVirtualAddress() + cbIdx * cbAlignedSize;
    cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
    cmd->SetGraphicsRootDescriptorTable(1, equirectSrvGpu);
    cmd->DrawInstanced(3, 1, 0, 0);
    cbIdx++;
  }

  // Transition env cubemap: RENDER_TARGET -> PIXEL_SHADER_RESOURCE.
  {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_envCubemap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
  }

  // ---- Pass 2: Irradiance Convolution ----
  cmd->SetPipelineState(m_irradiancePso.Get());
  cmd->SetGraphicsRootSignature(m_precomputeRootSig.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  for (uint32_t f = 0; f < 6; ++f) {
    fillCB(cbIdx, f, 0.0f, static_cast<float>(kEnvCubeSize));

    auto rtv = getRtv(6 + f); // irradiance cubemap face f
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    setViewport(kIrradianceSize);

    D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
        cbUpload->GetGPUVirtualAddress() + cbIdx * cbAlignedSize;
    cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
    cmd->SetGraphicsRootDescriptorTable(1, m_envCubeSrvGpu);
    cmd->DrawInstanced(3, 1, 0, 0);
    cbIdx++;
  }

  // Transition irradiance: RENDER_TARGET -> PIXEL_SHADER_RESOURCE.
  {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_irradianceCubemap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
  }

  // ---- Pass 3: Prefiltered Specular ----
  cmd->SetPipelineState(m_prefilteredPso.Get());
  cmd->SetGraphicsRootSignature(m_precomputeRootSig.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  for (uint32_t m = 0; m < kPrefilteredMips; ++m) {
    float roughness =
        static_cast<float>(m) / static_cast<float>(kPrefilteredMips - 1);
    uint32_t mipSize = kPrefilteredSize >> m;

    for (uint32_t f = 0; f < 6; ++f) {
      fillCB(cbIdx, f, roughness, static_cast<float>(kEnvCubeSize));

      // RTV index: 12 + m*6 + f
      auto rtv = getRtv(12 + m * 6 + f);
      cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      setViewport(mipSize);

      D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
          cbUpload->GetGPUVirtualAddress() + cbIdx * cbAlignedSize;
      cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
      cmd->SetGraphicsRootDescriptorTable(1, m_envCubeSrvGpu);
      cmd->DrawInstanced(3, 1, 0, 0);
      cbIdx++;
    }
  }

  // Transition prefiltered: RENDER_TARGET -> PIXEL_SHADER_RESOURCE.
  {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_prefilteredCubemap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
  }

  // ---- Pass 4: BRDF LUT ----
  cmd->SetPipelineState(m_brdfLutPso.Get());
  cmd->SetGraphicsRootSignature(m_precomputeRootSig.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  {
    // BRDF LUT doesn't need face matrices, but we still set a CB.
    IBLConvolveCB cb{};
    XMStoreFloat4x4(&cb.faceInvViewProj, XMMatrixIdentity());
    memcpy(cbMapped + cbIdx * cbAlignedSize, &cb, sizeof(IBLConvolveCB));

    auto rtv = getRtv(42); // BRDF LUT
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    setViewport(kBrdfLutSize);

    D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
        cbUpload->GetGPUVirtualAddress() + cbIdx * cbAlignedSize;
    cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
    // SRV table not used by BRDF LUT, but must be set for root sig
    // compatibility. Bind env cube (harmless).
    cmd->SetGraphicsRootDescriptorTable(1, m_envCubeSrvGpu);
    cmd->DrawInstanced(3, 1, 0, 0);
  }

  // Transition BRDF LUT: RENDER_TARGET -> PIXEL_SHADER_RESOURCE.
  {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_brdfLut.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
  }

  // Execute and wait.
  ThrowIfFailed(cmd->Close(), "IBL CmdList Close");
  ID3D12CommandList *lists[] = {cmd};
  dx.Queue()->ExecuteCommandLists(1, lists);
  dx.WaitForGpu();

  // Unmap CB upload buffer (will be released when ComPtr goes out of scope).
  cbUpload->Unmap(0, nullptr);
}
