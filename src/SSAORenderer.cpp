// ======================================
// File: SSAORenderer.cpp
// Purpose: SSAO renderer implementation (Phase 10.3).
//          Hemisphere kernel sampling + bilateral blur.
// ======================================

#include "SSAORenderer.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"

#include <DirectXPackedVector.h>
#include <cstring>
#include <d3dcompiler.h>
#include <random>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

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
      std::string msg(static_cast<const char *>(errors->GetBufferPointer()),
                      errors->GetBufferSize());
      throw std::runtime_error(msg);
    }
    ThrowIfFailed(hr, "D3DCompileFromFile (SSAO) failed");
  }
  return bytecode;
}

static float Lerp(float a, float b, float t) { return a + t * (b - a); }

void SSAORenderer::Initialize(DxContext &dx) {
  auto *dev = dx.Device();

  // ---- Generate hemisphere kernel samples ----
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
  std::uniform_real_distribution<float> distNeg(-1.0f, 1.0f);

  for (int i = 0; i < kMaxKernelSize; ++i) {
    // Random point in hemisphere (tangent space, +Z up)
    XMVECTOR sample = XMVectorSet(
        distNeg(rng), distNeg(rng), dist01(rng), 0.0f);
    sample = XMVector3Normalize(sample);
    // Scale: weight samples closer to the origin
    float scale = static_cast<float>(i) / static_cast<float>(kMaxKernelSize);
    scale = Lerp(0.1f, 1.0f, scale * scale);
    sample = XMVectorScale(sample, scale);
    XMStoreFloat4(&m_kernel[i], sample);
  }

  // ---- Create 4x4 noise texture ----
  {
    // 16 random XY rotation vectors, stored as R16G16B16A16_FLOAT.
    struct Half4 { uint16_t r, g, b, a; };
    Half4 noiseData[16];
    for (int i = 0; i < 16; ++i) {
      XMVECTOR v = XMVector3Normalize(
          XMVectorSet(distNeg(rng), distNeg(rng), 0.0f, 0.0f));
      XMFLOAT4 f;
      XMStoreFloat4(&f, v);
      noiseData[i].r = XMConvertFloatToHalf(f.x);
      noiseData[i].g = XMConvertFloatToHalf(f.y);
      noiseData[i].b = XMConvertFloatToHalf(0.0f);
      noiseData[i].a = XMConvertFloatToHalf(0.0f);
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 4;
    texDesc.Height = 4;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ThrowIfFailed(dev->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                      IID_PPV_ARGS(&m_noiseTex)),
                  "Create SSAO noise tex failed");

    // Upload buffer
    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                               &rowSize, &uploadSize);

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

    ThrowIfFailed(dev->CreateCommittedResource(
                      &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                      IID_PPV_ARGS(&m_noiseUpload)),
                  "Create SSAO noise upload buf failed");

    // Copy data
    void *mapped = nullptr;
    m_noiseUpload->Map(0, nullptr, &mapped);
    auto *dest = reinterpret_cast<uint8_t *>(mapped);
    const uint8_t *src = reinterpret_cast<const uint8_t *>(noiseData);
    for (UINT row = 0; row < numRows; ++row) {
      memcpy(dest + footprint.Offset + row * footprint.Footprint.RowPitch,
             src + row * (4 * sizeof(Half4)),
             4 * sizeof(Half4));
    }
    m_noiseUpload->Unmap(0, nullptr);

    // Execute copy
    auto &alloc = dx.m_frames[0].cmdAlloc;
    ThrowIfFailed(alloc->Reset(), "CmdAlloc Reset failed");
    ThrowIfFailed(dx.m_cmdList->Reset(alloc.Get(), nullptr),
                  "CmdList Reset failed");

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = m_noiseTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = m_noiseUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    dx.m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    dx.Transition(m_noiseTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ThrowIfFailed(dx.m_cmdList->Close(), "CmdList Close failed");
    ID3D12CommandList *lists[] = {dx.m_cmdList.Get()};
    dx.m_queue->ExecuteCommandLists(1, lists);
    dx.WaitForGpu();

    // Create SRV
    m_noiseSrvCpu = dx.AllocMainSrvCpu(1);
    m_noiseSrvGpu = dx.MainSrvGpuFromCpu(m_noiseSrvCpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(m_noiseTex.Get(), &srvDesc, m_noiseSrvCpu);
  }

  // ---- SSAO Root Signature ----
  // Param 0: Root CBV (b0) — SSAO constants (proj, invProj, kernel, params)
  // Param 1: SRV table (t0) — depth
  // Param 2: SRV table (t1) — view-space normals
  // Param 3: SRV table (t2) — noise texture
  // Static samplers: s0 = point clamp, s1 = point wrap
  {
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (int i = 0; i < 3; ++i) {
      ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[i].NumDescriptors = 1;
      ranges[i].BaseShaderRegister = static_cast<UINT>(i);
      ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    for (int i = 0; i < 3; ++i) {
      params[1 + i].ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
      params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    // s0: point clamp (depth + normals)
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: point wrap (noise tiling)
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(
                      &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
                  "Serialize SSAO RS failed");
    ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_ssaoRootSig)),
                  "Create SSAO RS failed");
  }

  // ---- Blur Root Signature ----
  // Param 0: 4 root constants (texelSizeX, texelSizeY, pad, pad)
  // Param 1: SRV table (t0) — raw SSAO
  // Param 2: SRV table (t1) — depth (bilateral weight)
  // Static sampler: s0 = point clamp
  {
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    for (int i = 0; i < 2; ++i) {
      ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[i].NumDescriptors = 1;
      ranges[i].BaseShaderRegister = static_cast<UINT>(i);
      ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    for (int i = 0; i < 2; ++i) {
      params[1 + i].ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
      params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(
                      &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
                  "Serialize SSAO Blur RS failed");
    ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_blurRootSig)),
                  "Create SSAO Blur RS failed");
  }

  // ---- Compile shaders ----
  auto ssaoVS =
      CompileShaderLocal(L"shaders/ssao.hlsl", "VSFullscreen", "vs_5_0");
  auto ssaoPS =
      CompileShaderLocal(L"shaders/ssao.hlsl", "PSGenerateSSAO", "ps_5_0");
  auto blurPS =
      CompileShaderLocal(L"shaders/ssao.hlsl", "PSBilateralBlur", "ps_5_0");

  // ---- SSAO PSO (fullscreen, R8_UNORM output) ----
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_ssaoRootSig.Get();
    pso.VS = {ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize()};
    pso.PS = {ssaoPS->GetBufferPointer(), ssaoPS->GetBufferSize()};

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
    pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_ssaoPso)),
        "Create SSAO PSO failed");
  }

  // ---- Blur PSO ----
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_blurRootSig.Get();
    pso.VS = {ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize()};
    pso.PS = {blurPS->GetBufferPointer(), blurPS->GetBufferSize()};

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
    pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_blurPso)),
        "Create SSAO Blur PSO failed");
  }
}

void SSAORenderer::ExecuteSSAO(DxContext &dx, const XMMATRIX &proj,
                                const XMMATRIX &view,
                                float radius, float bias, float power,
                                int kernelSize) {
  auto *cmd = dx.CmdList();

  // Constant buffer: proj, invProj, view, params, kernel samples.
  struct SSAOConstants {
    XMFLOAT4X4 proj;
    XMFLOAT4X4 invProj;
    XMFLOAT4X4 viewMat; // Phase 12.1: for world→view normal transform
    XMFLOAT4 params; // radius, bias, power, kernelSize
    XMFLOAT4 screenSize; // ssaoWidth, ssaoHeight, fullWidth, fullHeight
    XMFLOAT4 kernel[64];
  };

  SSAOConstants cb{};
  XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(proj));
  XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
  XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(invProj));
  XMStoreFloat4x4(&cb.viewMat, XMMatrixTranspose(view));
  cb.params = {radius, bias, power, static_cast<float>(kernelSize)};
  cb.screenSize = {static_cast<float>(dx.SsaoWidth()),
                   static_cast<float>(dx.SsaoHeight()),
                   static_cast<float>(dx.Width()),
                   static_cast<float>(dx.Height())};
  int count = (kernelSize > kMaxKernelSize) ? kMaxKernelSize : kernelSize;
  for (int i = 0; i < count; ++i)
    cb.kernel[i] = m_kernel[i];

  void *cbCpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(SSAOConstants), &cbCpu);
  memcpy(cbCpu, &cb, sizeof(SSAOConstants));

  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_ssaoRootSig.Get());
  cmd->SetPipelineState(m_ssaoPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Root param 0: CBV
  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  // Root param 1: depth SRV
  cmd->SetGraphicsRootDescriptorTable(1, dx.DepthSrvGpu());
  // Root param 2: normal SRV (G-buffer world-space normals, Phase 12.1)
  cmd->SetGraphicsRootDescriptorTable(2, dx.GBufferNormalSrvGpu());
  // Root param 3: noise SRV
  cmd->SetGraphicsRootDescriptorTable(3, m_noiseSrvGpu);

  // Set viewport to half-res SSAO target.
  D3D12_VIEWPORT vp{};
  vp.Width = static_cast<float>(dx.SsaoWidth());
  vp.Height = static_cast<float>(dx.SsaoHeight());
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor{0, 0, static_cast<LONG>(dx.SsaoWidth()),
                     static_cast<LONG>(dx.SsaoHeight())};
  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &scissor);

  auto rtv = dx.SsaoRtv();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

void SSAORenderer::ExecuteBlur(DxContext &dx) {
  auto *cmd = dx.CmdList();

  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_blurRootSig.Get());
  cmd->SetPipelineState(m_blurPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float texelX, texelY;
    float pad0, pad1;
  } blurCB;
  blurCB.texelX = 1.0f / static_cast<float>(dx.SsaoWidth());
  blurCB.texelY = 1.0f / static_cast<float>(dx.SsaoHeight());
  blurCB.pad0 = blurCB.pad1 = 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 4, &blurCB, 0);
  // Root param 1: raw SSAO SRV
  cmd->SetGraphicsRootDescriptorTable(1, dx.SsaoSrvGpu());
  // Root param 2: depth SRV (bilateral weight)
  cmd->SetGraphicsRootDescriptorTable(2, dx.DepthSrvGpu());

  D3D12_VIEWPORT vp{};
  vp.Width = static_cast<float>(dx.SsaoWidth());
  vp.Height = static_cast<float>(dx.SsaoHeight());
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor{0, 0, static_cast<LONG>(dx.SsaoWidth()),
                     static_cast<LONG>(dx.SsaoHeight())};
  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &scissor);

  auto rtv = dx.SsaoBlurRtv();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

std::string SSAORenderer::ReloadShaders(DxContext &dx) {
  std::string errors;

  // SSAO generation PSO
  if (m_ssaoRootSig) {
    auto vs = CompileShaderSafe(L"shaders/ssao.hlsl", "VSFullscreen", "vs_5_0");
    auto ps = CompileShaderSafe(L"shaders/ssao.hlsl", "PSGenerateSSAO", "ps_5_0");
    if (vs.success && ps.success) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_ssaoRootSig.Get();
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
      pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_ssaoPso = newPso;
    } else {
      if (!vs.success) errors += "[ssao.hlsl VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += "[ssao.hlsl SSAO PS] " + ps.errorMessage + "\n";
    }
  }

  // Bilateral blur PSO
  if (m_blurRootSig) {
    auto vs = CompileShaderSafe(L"shaders/ssao.hlsl", "VSFullscreen", "vs_5_0");
    auto ps = CompileShaderSafe(L"shaders/ssao.hlsl", "PSBilateralBlur", "ps_5_0");
    if (vs.success && ps.success) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
      pso.pRootSignature = m_blurRootSig.Get();
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
      pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
      pso.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> newPso;
      if (SUCCEEDED(dx.m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&newPso))))
        m_blurPso = newPso;
    } else {
      if (!vs.success) errors += "[ssao.hlsl blur VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += "[ssao.hlsl blur PS] " + ps.errorMessage + "\n";
    }
  }

  return errors;
}

void SSAORenderer::Reset() {
  m_ssaoRootSig.Reset();
  m_ssaoPso.Reset();
  m_blurRootSig.Reset();
  m_blurPso.Reset();
  m_noiseTex.Reset();
  m_noiseUpload.Reset();
}
