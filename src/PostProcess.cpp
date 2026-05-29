// ======================================
// File: PostProcess.cpp
// Purpose: Post-processing pipeline implementation (Phase 9).
//          Bloom (13-tap down / 9-tap tent up), ACES tonemap, FXAA 3.11.
// ======================================

#include "PostProcess.h"
#include "DxContext.h"
#include "DxUtil.h"
#include "ShaderCompiler.h"

#include <DirectXMath.h>
#include <cstring>
#include <d3dcompiler.h>
#include <stdexcept>

using namespace DirectX;

using Microsoft::WRL::ComPtr;

static ComPtr<ID3DBlob> CompileShader(const wchar_t *filePath,
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
    ThrowIfFailed(hr, "D3DCompileFromFile (PostProcess) failed");
  }
  return bytecode;
}

// Helper: create a root signature with 1 root constants slot + 1-2 SRV tables +
// bilinear clamp sampler.
static ComPtr<ID3D12RootSignature>
CreatePostProcessRootSig(ID3D12Device *device, uint32_t numSrvTables) {
  // Root param 0: 4 root constants (b0)
  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0; // b0
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_DESCRIPTOR_RANGE srvRanges[3]{};
  for (uint32_t i = 0; i < numSrvTables; ++i) {
    srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[i].NumDescriptors = 1;
    srvRanges[i].BaseShaderRegister = i; // t0, t1, ...
    srvRanges[i].RegisterSpace = 0;
    srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

    params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
    params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  samp.ShaderRegister = 0; // s0
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 1 + numSrvTables;
  desc.pParameters = params;
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &samp;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ComPtr<ID3DBlob> blob, err;
  ThrowIfFailed(
      D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
      "Serialize PostProcess RS failed");
  ComPtr<ID3D12RootSignature> rs;
  ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(),
                                            blob->GetBufferSize(),
                                            IID_PPV_ARGS(&rs)),
                "Create PostProcess RS failed");
  return rs;
}

// Helper: create fullscreen PSO (no input layout, depth disabled).
static ComPtr<ID3D12PipelineState>
CreateFullscreenPSO(ID3D12Device *device, ID3D12RootSignature *rootSig,
                    ID3DBlob *vs, ID3DBlob *ps, DXGI_FORMAT rtvFormat,
                    bool additiveBlend = false) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = rootSig;
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (additiveBlend) {
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;

  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_NONE;
  rast.DepthClipEnable = FALSE;
  pso.RasterizerState = rast;

  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = FALSE;
  ds.StencilEnable = FALSE;
  pso.DepthStencilState = ds;

  pso.InputLayout = {nullptr, 0};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = rtvFormat;
  pso.SampleDesc.Count = 1;

  ComPtr<ID3D12PipelineState> result;
  ThrowIfFailed(
      device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&result)),
      "Create PostProcess PSO failed");
  return result;
}

void PostProcessRenderer::Initialize(DxContext &dx) {
  auto *dev = dx.Device();

  // ---- Bloom ----
  m_bloomRootSig = CreatePostProcessRootSig(dev, 1);

  auto bloomVS = CompileShader(L"shaders/bloom.hlsl", "VSFullscreen", "vs_5_0");
  auto bloomDownPS =
      CompileShader(L"shaders/bloom.hlsl", "PSDownsample", "ps_5_0");
  auto bloomUpPS =
      CompileShader(L"shaders/bloom.hlsl", "PSUpsample", "ps_5_0");

  m_bloomDownPso = CreateFullscreenPSO(dev, m_bloomRootSig.Get(), bloomVS.Get(),
                                       bloomDownPS.Get(), dx.HdrFormat());
  m_bloomUpPso = CreateFullscreenPSO(dev, m_bloomRootSig.Get(), bloomVS.Get(),
                                     bloomUpPS.Get(), dx.HdrFormat(), true);

  // ---- Tonemap (3 SRVs: HDR scene t0, bloom t1, AO t2) ----
  m_tonemapRootSig = CreatePostProcessRootSig(dev, 3);

  auto tonemapVS =
      CompileShader(L"shaders/postprocess.hlsl", "VSFullscreen", "vs_5_0");
  auto tonemapPS =
      CompileShader(L"shaders/postprocess.hlsl", "PSMain", "ps_5_0");

  m_tonemapPso = CreateFullscreenPSO(dev, m_tonemapRootSig.Get(),
                                     tonemapVS.Get(), tonemapPS.Get(),
                                     dx.BackBufferFormat());

  // ---- FXAA ----
  m_fxaaRootSig = CreatePostProcessRootSig(dev, 1);

  auto fxaaVS = CompileShader(L"shaders/fxaa.hlsl", "VSFullscreen", "vs_5_0");
  auto fxaaPS = CompileShader(L"shaders/fxaa.hlsl", "PSMain", "ps_5_0");

  m_fxaaPso = CreateFullscreenPSO(dev, m_fxaaRootSig.Get(), fxaaVS.Get(),
                                  fxaaPS.Get(), dx.BackBufferFormat());

  // ---- Velocity generation (Phase 10.5) ----
  // Custom root sig: root CBV (b0) + 1 SRV table (t0) + point sampler
  {
    D3D12_ROOT_PARAMETER velParams[2]{};
    velParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    velParams[0].Descriptor.ShaderRegister = 0;
    velParams[0].Descriptor.RegisterSpace = 0;
    velParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE velRange{};
    velRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    velRange.NumDescriptors = 1;
    velRange.BaseShaderRegister = 0;
    velRange.RegisterSpace = 0;
    velRange.OffsetInDescriptorsFromTableStart = 0;

    velParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    velParams[1].DescriptorTable.NumDescriptorRanges = 1;
    velParams[1].DescriptorTable.pDescriptorRanges = &velRange;
    velParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC velSamp{};
    velSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    velSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    velSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    velSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    velSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    velSamp.MaxLOD = D3D12_FLOAT32_MAX;
    velSamp.ShaderRegister = 0;
    velSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC velDesc{};
    velDesc.NumParameters = 2;
    velDesc.pParameters = velParams;
    velDesc.NumStaticSamplers = 1;
    velDesc.pStaticSamplers = &velSamp;
    velDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&velDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, &err),
        "Serialize Velocity RS failed");
    ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_velocityRootSig)),
                  "Create Velocity RS failed");
  }

  auto velVS =
      CompileShader(L"shaders/velocity.hlsl", "VSFullscreen", "vs_5_0");
  auto velPS = CompileShader(L"shaders/velocity.hlsl", "PSMain", "ps_5_0");

  m_velocityPso =
      CreateFullscreenPSO(dev, m_velocityRootSig.Get(), velVS.Get(),
                          velPS.Get(), DXGI_FORMAT_R16G16_FLOAT);

  // ---- Motion blur (Phase 10.5) ----
  m_motionBlurRootSig = CreatePostProcessRootSig(dev, 2);

  auto mbVS =
      CompileShader(L"shaders/motionblur.hlsl", "VSFullscreen", "vs_5_0");
  auto mbPS = CompileShader(L"shaders/motionblur.hlsl", "PSMain", "ps_5_0");

  m_motionBlurPso =
      CreateFullscreenPSO(dev, m_motionBlurRootSig.Get(), mbVS.Get(),
                          mbPS.Get(), dx.BackBufferFormat());

  // ---- DOF (Phase 10.6) ----
  // Custom root sig: 8 root constants (b0) + 2 SRV tables (t0 color, t1 depth)
  {
    D3D12_ROOT_PARAMETER dofParams[3]{};
    dofParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    dofParams[0].Constants.ShaderRegister = 0; // b0
    dofParams[0].Constants.RegisterSpace = 0;
    dofParams[0].Constants.Num32BitValues = 8;
    dofParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE dofRanges[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      dofRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      dofRanges[i].NumDescriptors = 1;
      dofRanges[i].BaseShaderRegister = i; // t0, t1
      dofRanges[i].RegisterSpace = 0;
      dofRanges[i].OffsetInDescriptorsFromTableStart = 0;

      dofParams[1 + i].ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dofParams[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      dofParams[1 + i].DescriptorTable.pDescriptorRanges = &dofRanges[i];
      dofParams[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC dofSamp{};
    dofSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    dofSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    dofSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    dofSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    dofSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    dofSamp.MaxLOD = D3D12_FLOAT32_MAX;
    dofSamp.ShaderRegister = 0; // s0
    dofSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC dofDesc{};
    dofDesc.NumParameters = 3;
    dofDesc.pParameters = dofParams;
    dofDesc.NumStaticSamplers = 1;
    dofDesc.pStaticSamplers = &dofSamp;
    dofDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(
                      &dofDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
                  "Serialize DOF RS failed");
    ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_dofRootSig)),
                  "Create DOF RS failed");
  }

  auto dofVS = CompileShader(L"shaders/dof.hlsl", "VSFullscreen", "vs_5_0");
  auto dofPS = CompileShader(L"shaders/dof.hlsl", "PSDof", "ps_5_0");

  m_dofPso = CreateFullscreenPSO(dev, m_dofRootSig.Get(), dofVS.Get(),
                                 dofPS.Get(), dx.BackBufferFormat());

  // ---- TAA (Phase 10.4) ----
  m_taaRootSig = CreatePostProcessRootSig(dev, 3); // t0=current HDR, t1=history, t2=velocity

  auto taaVS = CompileShader(L"shaders/taa.hlsl", "VSFullscreen", "vs_5_0");
  auto taaPS = CompileShader(L"shaders/taa.hlsl", "PSMain", "ps_5_0");

  m_taaPso = CreateFullscreenPSO(dev, m_taaRootSig.Get(), taaVS.Get(),
                                 taaPS.Get(), dx.HdrFormat());
}

void PostProcessRenderer::ExecuteBloom(DxContext &dx,
                                       const PostProcessParams &params) {
  if (!params.bloomEnabled)
    return;

  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_bloomRootSig.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // ---- Downsample chain ----
  // First pass: HDR scene -> bloom mip 0
  // Subsequent passes: bloom mip[i-1] -> bloom mip[i]
  for (uint32_t i = 0; i < DxContext::kBloomMips; ++i) {
    const auto &mip = dx.GetBloomMip(i);

    // Source: either HDR target (first pass) or previous bloom mip
    if (i == 0) {
      // HDR target (or TAA output override) is already in SRV state.
      auto hdrSrv = (params.hdrOverrideSrvGpu.ptr != 0)
                         ? params.hdrOverrideSrvGpu
                         : dx.HdrSrvGpu();
      cmd->SetGraphicsRootDescriptorTable(1, hdrSrv);
    } else {
      // Previous bloom mip: RT -> SRV
      dx.Transition(dx.GetBloomMip(i - 1).tex.Get(),
                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      cmd->SetGraphicsRootDescriptorTable(1, dx.GetBloomMip(i - 1).srvGpu);
    }

    // Destination: current bloom mip (SRV -> RT)
    dx.Transition(mip.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Determine source texel size
    float srcW, srcH;
    if (i == 0) {
      srcW = static_cast<float>(dx.Width());
      srcH = static_cast<float>(dx.Height());
    } else {
      srcW = static_cast<float>(dx.GetBloomMip(i - 1).width);
      srcH = static_cast<float>(dx.GetBloomMip(i - 1).height);
    }

    struct {
      float texelX, texelY;
      float threshold;
      float pad;
    } bloomCB;
    bloomCB.texelX = 1.0f / srcW;
    bloomCB.texelY = 1.0f / srcH;
    bloomCB.threshold = (i == 0) ? params.bloomThreshold : 0.0f;
    bloomCB.pad = 0.0f;

    cmd->SetGraphicsRoot32BitConstants(0, 4, &bloomCB, 0);
    cmd->SetPipelineState(m_bloomDownPso.Get());

    // Set viewport/scissor to mip dimensions
    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(mip.width);
    vp.Height = static_cast<float>(mip.height);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(mip.width),
                       static_cast<LONG>(mip.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->OMSetRenderTargets(1, &mip.rtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
  }

  // Transition last bloom mip RT -> SRV
  dx.Transition(
      dx.GetBloomMip(DxContext::kBloomMips - 1).tex.Get(),
      D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // ---- Upsample chain (additive blend) ----
  // From mip[kBloomMips-1] up to mip[0].
  // Each pass reads mip[i+1] and renders additively into mip[i].
  cmd->SetPipelineState(m_bloomUpPso.Get());

  for (int i = static_cast<int>(DxContext::kBloomMips) - 2; i >= 0; --i) {
    const auto &dst = dx.GetBloomMip(static_cast<uint32_t>(i));
    const auto &src = dx.GetBloomMip(static_cast<uint32_t>(i + 1));

    // src is already SRV (transitioned in downsample or previous upsample)
    cmd->SetGraphicsRootDescriptorTable(1, src.srvGpu);

    // dst: was left as RT from downsample (or need to check state)
    // After downsample, all mips except last are still RT.
    // Actually, mips 0..N-2 were transitioned to SRV during downsample (for next pass).
    // Only the last one was transitioned RT->SRV explicitly.
    // Wait - let me re-check: during downsample:
    //   mip[i] goes SRV->RT at start of its pass
    //   mip[i-1] (source) goes RT->SRV at start of pass i
    // So after downsample, mips 0..N-2 are SRV, mip N-1 is SRV (explicitly transitioned)
    // For upsample, dst needs to be RT (with additive blend on existing content).
    dx.Transition(dst.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

    struct {
      float texelX, texelY;
      float threshold;
      float pad;
    } upCB;
    upCB.texelX = 1.0f / static_cast<float>(src.width);
    upCB.texelY = 1.0f / static_cast<float>(src.height);
    upCB.threshold = 0.0f;
    upCB.pad = 0.0f;

    cmd->SetGraphicsRoot32BitConstants(0, 4, &upCB, 0);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(dst.width);
    vp.Height = static_cast<float>(dst.height);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(dst.width),
                       static_cast<LONG>(dst.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->OMSetRenderTargets(1, &dst.rtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Transition dst RT -> SRV for next iteration (or tonemap read)
    dx.Transition(dst.tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  // After upsample, bloom mip 0 is in SRV state — ready for tonemap.
}

void PostProcessRenderer::ExecuteTonemap(DxContext &dx,
                                         const PostProcessParams &params) {
  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_tonemapRootSig.Get());
  cmd->SetPipelineState(m_tonemapPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float exposure;
    float bloomIntensity;
    float aoStrength;
    float pad;
  } cb;
  cb.exposure = params.exposure;
  cb.bloomIntensity = params.bloomEnabled ? params.bloomIntensity : 0.0f;
  cb.aoStrength = params.aoStrength;
  cb.pad = 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);

  // HDR scene SRV (or TAA output override, already in SRV state after bloom)
  auto hdrSrv = (params.hdrOverrideSrvGpu.ptr != 0)
                    ? params.hdrOverrideSrvGpu
                    : dx.HdrSrvGpu();
  cmd->SetGraphicsRootDescriptorTable(1, hdrSrv);
  // Bloom result (mip 0) SRV
  cmd->SetGraphicsRootDescriptorTable(2, dx.GetBloomMip(0).srvGpu);
  // AO blur result SRV
  cmd->SetGraphicsRootDescriptorTable(3, params.aoSrvGpu);

  // Output target: LDR if FXAA, DOF, or motion blur enabled, backbuffer if not.
  const bool needLdr =
      params.fxaaEnabled || params.motionBlurEnabled || params.dofEnabled;
  D3D12_CPU_DESCRIPTOR_HANDLE outRtv;
  if (needLdr) {
    dx.Transition(dx.LdrTarget(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    outRtv = dx.LdrRtv();
  } else {
    outRtv = dx.CurrentRtv();
  }

  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &outRtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);

  if (needLdr) {
    dx.Transition(dx.LdrTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
}

void PostProcessRenderer::ExecuteFXAA(DxContext &dx,
                                      const PostProcessParams &params) {
  if (!params.fxaaEnabled)
    return;

  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_fxaaRootSig.Get());
  cmd->SetPipelineState(m_fxaaPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float rcpFrameX, rcpFrameY;
    float pad0, pad1;
  } cb;
  cb.rcpFrameX = 1.0f / static_cast<float>(dx.Width());
  cb.rcpFrameY = 1.0f / static_cast<float>(dx.Height());
  cb.pad0 = cb.pad1 = 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);
  // Read from LDR2 if motion blur is active, DOF target if DOF is active,
  // otherwise LDR.
  if (params.motionBlurEnabled) {
    cmd->SetGraphicsRootDescriptorTable(1, dx.Ldr2SrvGpu());
  } else if (params.dofEnabled) {
    cmd->SetGraphicsRootDescriptorTable(1, dx.DofSrvGpu());
  } else {
    cmd->SetGraphicsRootDescriptorTable(1, dx.LdrSrvGpu());
  }

  // Output to backbuffer
  auto rtv = dx.CurrentRtv();
  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================================
// Velocity generation (Phase 10.5)
// ============================================================================
void PostProcessRenderer::ExecuteVelocityGen(
    DxContext &dx, const XMMATRIX &invViewProj, const XMMATRIX &prevViewProj) {
  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_velocityRootSig.Get());
  cmd->SetPipelineState(m_velocityPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Upload two matrices to frame constant ring (transposed for HLSL column-major).
  struct {
    XMFLOAT4X4 invVP;
    XMFLOAT4X4 prevVP;
  } cbData;
  XMStoreFloat4x4(&cbData.invVP, XMMatrixTranspose(invViewProj));
  XMStoreFloat4x4(&cbData.prevVP, XMMatrixTranspose(prevViewProj));

  void *mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
      dx.AllocFrameConstants(sizeof(cbData), &mapped);
  std::memcpy(mapped, &cbData, sizeof(cbData));

  cmd->SetGraphicsRootConstantBufferView(0, cbGpu);
  cmd->SetGraphicsRootDescriptorTable(1, dx.DepthSrvGpu());

  auto rtv = dx.VelocityRtv();
  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================================
// Motion blur (Phase 10.5)
// ============================================================================
void PostProcessRenderer::ExecuteMotionBlur(DxContext &dx,
                                            const PostProcessParams &params) {
  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_motionBlurRootSig.Get());
  cmd->SetPipelineState(m_motionBlurPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float strength;
    float invSampleCount;
    float pad0, pad1;
  } cb;
  cb.strength = params.motionBlurStrength;
  cb.invSampleCount =
      1.0f / static_cast<float>((params.motionBlurSamples > 0)
                                     ? params.motionBlurSamples
                                     : 1);
  cb.pad0 = cb.pad1 = 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);
  // Read from DOF target if DOF is active, otherwise LDR.
  if (params.dofEnabled) {
    cmd->SetGraphicsRootDescriptorTable(1, dx.DofSrvGpu());
  } else {
    cmd->SetGraphicsRootDescriptorTable(1, dx.LdrSrvGpu());
  }
  cmd->SetGraphicsRootDescriptorTable(2, dx.VelocitySrvGpu()); // velocity

  auto rtv = dx.Ldr2Rtv();
  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================================
// Depth of Field (Phase 10.6)
// ============================================================================
void PostProcessRenderer::ExecuteDOF(DxContext &dx,
                                     const PostProcessParams &params) {
  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_dofRootSig.Get());
  cmd->SetPipelineState(m_dofPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float focalDistance;
    float focalRange;
    float maxBlur;
    float nearZ;
    float farZ;
    float texelSizeX;
    float texelSizeY;
    float pad;
  } cb;
  cb.focalDistance = params.dofFocalDistance;
  cb.focalRange = params.dofFocalRange;
  cb.maxBlur = params.dofMaxBlur;
  cb.nearZ = params.nearZ;
  cb.farZ = params.farZ;
  cb.texelSizeX = 1.0f / static_cast<float>(dx.Width());
  cb.texelSizeY = 1.0f / static_cast<float>(dx.Height());
  cb.pad = 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 8, &cb, 0);
  cmd->SetGraphicsRootDescriptorTable(1, dx.LdrSrvGpu());   // LDR color
  cmd->SetGraphicsRootDescriptorTable(2, dx.DepthSrvGpu()); // depth

  auto rtv = dx.DofRtv();
  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================================
// TAA resolve (Phase 10.4)
// ============================================================================
void PostProcessRenderer::ExecuteTAA(DxContext &dx,
                                     const PostProcessParams &params) {
  auto *cmd = dx.CmdList();
  ID3D12DescriptorHeap *heaps[] = {dx.MainSrvHeap()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(m_taaRootSig.Get());
  cmd->SetPipelineState(m_taaPso.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  struct {
    float texelX, texelY;
    float blendFactor;
    float firstFrame;
  } cb;
  cb.texelX = 1.0f / static_cast<float>(dx.Width());
  cb.texelY = 1.0f / static_cast<float>(dx.Height());
  cb.blendFactor = params.taaBlendFactor;
  cb.firstFrame = dx.TaaFirstFrame() ? 1.0f : 0.0f;

  cmd->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);
  cmd->SetGraphicsRootDescriptorTable(1, dx.HdrSrvGpu());           // t0: current HDR
  cmd->SetGraphicsRootDescriptorTable(2, dx.TaaHistorySrvGpu());     // t1: previous TAA output
  cmd->SetGraphicsRootDescriptorTable(3, dx.VelocitySrvGpu());       // t2: velocity

  auto rtv = dx.TaaOutputRtv();
  dx.SetViewportScissorFull();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

// Safe fullscreen PSO creation — returns nullptr on failure instead of throwing.
static ComPtr<ID3D12PipelineState>
CreateFullscreenPSOSafe(ID3D12Device *device, ID3D12RootSignature *rootSig,
                        ID3DBlob *vs, ID3DBlob *ps, DXGI_FORMAT rtvFormat,
                        bool additiveBlend = false) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = rootSig;
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (additiveBlend) {
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  pso.BlendState = blend;
  pso.SampleMask = UINT_MAX;
  D3D12_RASTERIZER_DESC rast{};
  rast.FillMode = D3D12_FILL_MODE_SOLID;
  rast.CullMode = D3D12_CULL_MODE_NONE;
  rast.DepthClipEnable = FALSE;
  pso.RasterizerState = rast;
  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = FALSE;
  ds.StencilEnable = FALSE;
  pso.DepthStencilState = ds;
  pso.InputLayout = {nullptr, 0};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = rtvFormat;
  pso.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> result;
  device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&result));
  return result;
}

std::string PostProcessRenderer::ReloadShaders(DxContext &dx) {
  std::string errors;
  auto *dev = dx.Device();

  // Helper: compile VS+PS, rebuild PSO, assign if success.
  auto reloadPair = [&](const wchar_t *file, const char *vsEntry, const char *psEntry,
                        const char *vsTarget, const char *psTarget,
                        ID3D12RootSignature *rs, DXGI_FORMAT fmt,
                        ComPtr<ID3D12PipelineState> &target, bool additive = false) {
    auto vs = CompileShaderSafe(file, vsEntry, vsTarget);
    auto ps = CompileShaderSafe(file, psEntry, psTarget);
    if (vs.success && ps.success) {
      auto newPso = CreateFullscreenPSOSafe(dev, rs, vs.bytecode.Get(), ps.bytecode.Get(), fmt, additive);
      if (newPso) target = newPso;
    } else {
      if (!vs.success) errors += std::string("[") + psEntry + " VS] " + vs.errorMessage + "\n";
      if (!ps.success) errors += std::string("[") + psEntry + " PS] " + ps.errorMessage + "\n";
    }
  };

  // Bloom
  reloadPair(L"shaders/bloom.hlsl", "VSFullscreen", "PSDownsample", "vs_5_0", "ps_5_0",
             m_bloomRootSig.Get(), dx.HdrFormat(), m_bloomDownPso);
  reloadPair(L"shaders/bloom.hlsl", "VSFullscreen", "PSUpsample", "vs_5_0", "ps_5_0",
             m_bloomRootSig.Get(), dx.HdrFormat(), m_bloomUpPso, true);

  // Tonemap
  reloadPair(L"shaders/postprocess.hlsl", "VSFullscreen", "PSMain", "vs_5_0", "ps_5_0",
             m_tonemapRootSig.Get(), dx.BackBufferFormat(), m_tonemapPso);

  // FXAA
  reloadPair(L"shaders/fxaa.hlsl", "VSFullscreen", "PSMain", "vs_5_0", "ps_5_0",
             m_fxaaRootSig.Get(), dx.BackBufferFormat(), m_fxaaPso);

  // Velocity
  reloadPair(L"shaders/velocity.hlsl", "VSFullscreen", "PSMain", "vs_5_0", "ps_5_0",
             m_velocityRootSig.Get(), DXGI_FORMAT_R16G16_FLOAT, m_velocityPso);

  // Motion blur
  reloadPair(L"shaders/motionblur.hlsl", "VSFullscreen", "PSMain", "vs_5_0", "ps_5_0",
             m_motionBlurRootSig.Get(), dx.BackBufferFormat(), m_motionBlurPso);

  // DOF
  reloadPair(L"shaders/dof.hlsl", "VSFullscreen", "PSDof", "vs_5_0", "ps_5_0",
             m_dofRootSig.Get(), dx.BackBufferFormat(), m_dofPso);

  // TAA
  reloadPair(L"shaders/taa.hlsl", "VSFullscreen", "PSMain", "vs_5_0", "ps_5_0",
             m_taaRootSig.Get(), dx.HdrFormat(), m_taaPso);

  return errors;
}

void PostProcessRenderer::Reset() {
  m_bloomRootSig.Reset();
  m_bloomDownPso.Reset();
  m_bloomUpPso.Reset();
  m_tonemapRootSig.Reset();
  m_tonemapPso.Reset();
  m_fxaaRootSig.Reset();
  m_fxaaPso.Reset();
  m_velocityRootSig.Reset();
  m_velocityPso.Reset();
  m_motionBlurRootSig.Reset();
  m_motionBlurPso.Reset();
  m_dofRootSig.Reset();
  m_dofPso.Reset();
  m_taaRootSig.Reset();
  m_taaPso.Reset();
}
