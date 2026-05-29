// ======================================
// File: DxContext.cpp
// Purpose: DirectX 12 device context implementation. Phase 8: trimmed to
//          device/swapchain/heap management + frame lifecycle. Rendering logic
//          now lives in renderer modules (MeshRenderer, ShadowMap, SkyRenderer,
//          GridRenderer, ParticleRenderer) and render passes (RenderPasses.cpp).
// ======================================

#include "DxContext.h"
#include "DxUtil.h"
#include "MeshRenderer.h"
#include "ParticleRenderer.h"
#include "ShadowMap.h"

#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

static UINT Align256(UINT size) { return (size + 255u) & ~255u; }

static constexpr uint32_t kFrameConstantsBytes = 2 * 1024 * 1024; // 2MB per frame

static void EnableDebugLayerIfRequested(bool enable) {
  if (!enable)
    return;

  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
    debug->EnableDebugLayer();
  }
}

DxContext::DxContext() = default;
DxContext::~DxContext() = default;

void DxContext::Initialize(HWND hwnd, uint32_t width, uint32_t height,
                           bool enableDebugLayer) {
  m_enableDebugLayer = enableDebugLayer;
  m_width = width;
  m_height = height;

  EnableDebugLayerIfRequested(enableDebugLayer);

  ThrowIfFailed(
      CreateDXGIFactory2(enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0,
                         IID_PPV_ARGS(&m_factory)),
      "CreateDXGIFactory2 failed");

  CreateDeviceAndQueue(enableDebugLayer);
  CreateMainSrvHeap();
  CreateImGuiResources();
  CreateCommandObjects();
  CreateSwapChain(hwnd);
  CreateRtvHeapAndViews();
  CreateDepthResources();
  CreatePostProcessResources();

  // Shadows v1: create a single directional shadow map (fixed resolution).
  m_shadowMap = std::make_unique<ShadowMap>();
  m_shadowMap->Initialize(*this, 2048);

  CreateFence();

  // Eagerly create rendering modules (GPU init happens later).
  m_meshRenderer = std::make_unique<MeshRenderer>();
}

void DxContext::Shutdown() {
  WaitForGpu();

  // Release renderer modules before tearing down core DX12 objects.
  if (m_particleRenderer)
    m_particleRenderer->Reset();
  m_particleRenderer.reset();
  m_meshRenderer.reset();
  m_shadowMap.reset();

  for (auto &fr : m_frames) {
    if (fr.constants && fr.constantsMapped) {
      fr.constants->Unmap(0, nullptr);
      fr.constantsMapped = nullptr;
    }
    fr.constantsOffset = 0;
  }

  if (m_fenceEvent) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
}

void DxContext::CreateDeviceAndQueue(bool enableDebugLayer) {
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;
       m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;

    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&m_device))))
      break;
  }

  if (!m_device) {
    // WARP fallback
    ComPtr<IDXGIAdapter> warp;
    ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),
                  "EnumWarpAdapter failed");
    ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&m_device)),
                  "D3D12CreateDevice (WARP) failed");
  }

  if (enableDebugLayer) {
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_device.As(&infoQueue))) {
      infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
  }

  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  q.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  ThrowIfFailed(m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue)),
                "CreateCommandQueue failed");
}

// ---- Convenience wrappers ----

uint32_t DxContext::CreateMeshResources(const LoadedMesh &mesh,
                                       const MaterialImages &images,
                                       const Material &material) {
  return m_meshRenderer->CreateMeshResources(*this, mesh, images, material);
}

void DxContext::ReplaceMeshTexture(uint32_t meshId, uint32_t slot,
                                   const LoadedImage &img) {
  m_meshRenderer->ReplaceMeshTexture(*this, meshId, slot, img);
}

void DxContext::ClearMeshTexture(uint32_t meshId, uint32_t slot) {
  m_meshRenderer->ClearMeshTexture(*this, meshId, slot);
}

D3D12_GPU_DESCRIPTOR_HANDLE DxContext::GetTextureImGuiSrv(uint32_t meshId,
                                                          uint32_t slot) {
  return m_meshRenderer->GetTextureImGuiSrv(*this, meshId, slot);
}

void DxContext::InitParticleRenderer() {
  if (!m_particleRenderer)
    m_particleRenderer = std::make_unique<ParticleRenderer>();
  m_particleRenderer->Initialize(*this);
}

// ---- New Phase 8 helpers ----

void DxContext::Transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = res;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_cmdList->ResourceBarrier(1, &barrier);
}

void DxContext::SetViewportScissorFull() {
  D3D12_VIEWPORT vp{};
  vp.TopLeftX = 0.0f;
  vp.TopLeftY = 0.0f;
  vp.Width = static_cast<float>(m_width);
  vp.Height = static_cast<float>(m_height);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;

  D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width),
                     static_cast<LONG>(m_height)};

  m_cmdList->RSSetViewports(1, &vp);
  m_cmdList->RSSetScissorRects(1, &scissor);
}

MeshRenderer &DxContext::GetMeshRenderer() {
  if (!m_meshRenderer)
    throw std::runtime_error("GetMeshRenderer: not initialized");
  return *m_meshRenderer;
}

ShadowMap &DxContext::GetShadowMap() {
  if (!m_shadowMap)
    throw std::runtime_error("GetShadowMap: not initialized");
  return *m_shadowMap;
}

ParticleRenderer &DxContext::GetParticleRenderer() {
  if (!m_particleRenderer)
    throw std::runtime_error("GetParticleRenderer: not initialized");
  return *m_particleRenderer;
}

// ---- Descriptor heap management ----

void DxContext::CreateImGuiResources() {
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 256;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ThrowIfFailed(
      m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_imguiSrvHeap)),
      "CreateDescriptorHeap ImGui SRV failed");

  m_imguiSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_imguiSrvCapacity = heap.NumDescriptors;
  m_imguiSrvNext = 0;
}

void DxContext::CreateMainSrvHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 4096;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ThrowIfFailed(
      m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_mainSrvHeap)),
      "CreateDescriptorHeap Main SRV failed");

  m_mainSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_mainSrvCapacity = heap.NumDescriptors;
  m_mainSrvNext = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE DxContext::AllocMainSrvCpu(uint32_t count) {
  if (!m_mainSrvHeap)
    throw std::runtime_error("AllocMainSrvCpu: main SRV heap not created");
  if (count == 0)
    throw std::runtime_error("AllocMainSrvCpu: count must be > 0");
  if (m_mainSrvNext + count > m_mainSrvCapacity)
    throw std::runtime_error(
        "AllocMainSrvCpu: main SRV heap out of descriptors");

  D3D12_CPU_DESCRIPTOR_HANDLE cpu =
      m_mainSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(m_mainSrvNext) *
             static_cast<SIZE_T>(m_mainSrvDescriptorSize);
  m_mainSrvNext += count;
  return cpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE
DxContext::MainSrvGpuFromCpu(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
  const SIZE_T baseCpu =
      m_mainSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
  const SIZE_T delta = cpu.ptr - baseCpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu =
      m_mainSrvHeap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(delta);
  return gpu;
}

// ---- Command objects + per-frame constant ring ----

void DxContext::CreateCommandObjects() {
  for (uint32_t i = 0; i < FrameCount; ++i) {
    ThrowIfFailed(
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&m_frames[i].cmdAlloc)),
        "CreateCommandAllocator failed");
  }
  ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            m_frames[0].cmdAlloc.Get(), nullptr,
                                            IID_PPV_ARGS(&m_cmdList)),
                "CreateCommandList failed");

  // Start closed; BeginFrame will Reset.
  ThrowIfFailed(m_cmdList->Close(), "CommandList Close failed");

  // Per-frame constant buffer ring (upload heap).
  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC resDesc{};
  resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  resDesc.Width = kFrameConstantsBytes;
  resDesc.Height = 1;
  resDesc.DepthOrArraySize = 1;
  resDesc.MipLevels = 1;
  resDesc.SampleDesc.Count = 1;
  resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  D3D12_RANGE range{0, 0};
  for (uint32_t i = 0; i < FrameCount; ++i) {
    ThrowIfFailed(m_device->CreateCommittedResource(
                      &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                      IID_PPV_ARGS(&m_frames[i].constants)),
                  "CreateCommittedResource (FrameConstants) failed");
    ThrowIfFailed(
        m_frames[i].constants->Map(
            0, &range,
            reinterpret_cast<void **>(&m_frames[i].constantsMapped)),
        "FrameConstants Map failed");
    m_frames[i].constantsOffset = 0;
  }
}

// ---- Swap chain / RTV / Depth ----

void DxContext::CreateSwapChain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = m_width;
  sc.Height = m_height;
  sc.Format = m_backBufferFormat;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = FrameCount;
  sc.SampleDesc.Count = 1;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sc.Scaling = DXGI_SCALING_STRETCH;

  ComPtr<IDXGISwapChain1> swapchain1;
  ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
                    m_queue.Get(), hwnd, &sc, nullptr, nullptr, &swapchain1),
                "CreateSwapChainForHwnd failed");

  ThrowIfFailed(swapchain1.As(&m_swapchain), "Swapchain cast failed");
  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

  // Suppress DXGI's built-in Alt+Enter (exclusive fullscreen) —
  // we use borderless fullscreen instead (Phase 12.6).
  m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
}

void DxContext::CreateRtvHeapAndViews() {
  // 2 backbuffers + 1 HDR + 1 LDR + 5 bloom mips + 3 SSAO + 2 motion blur + 1 DOF + 4 G-buffer + 2 TAA = 21
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = FrameCount + 1 + 1 + kBloomMips + 3 + 2 + 1 + 4 + 2;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ThrowIfFailed(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap)),
                "CreateDescriptorHeap RTV failed");

  m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  RecreateSizeDependentResources();
}

void DxContext::RecreateSizeDependentResources() {
  for (auto &bb : m_backBuffers)
    bb.Reset();

  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < FrameCount; ++i) {
    ThrowIfFailed(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
                  "Swapchain GetBuffer failed");
    m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
    handle.ptr += m_rtvDescriptorSize;
  }
}

void DxContext::CreateDepthResources() {
  if (!m_dsvHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(
        m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_dsvHeap)),
        "CreateDescriptorHeap DSV failed");
  }

  m_depthBuffer.Reset();

  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = m_width;
  desc.Height = m_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  // Use TYPELESS so we can create both DSV (D32_FLOAT) and SRV (R32_FLOAT).
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = m_depthFormat;
  clear.DepthStencil.Depth = 1.0f;
  clear.DepthStencil.Stencil = 0;

  ThrowIfFailed(
      m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        &clear, IID_PPV_ARGS(&m_depthBuffer)),
      "CreateCommittedResource (Depth) failed");

  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = m_depthFormat;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dsv.Flags = D3D12_DSV_FLAG_NONE;
  m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsv, Dsv());

  // SRV for reading depth in SSAO pass (allocate handle once, recreate view on resize).
  if (!m_depthSrvAllocated) {
    m_depthSrvCpu = AllocMainSrvCpu(1);
    m_depthSrvGpu = MainSrvGpuFromCpu(m_depthSrvCpu);
    m_depthSrvAllocated = true;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
  depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
  depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  depthSrv.Texture2D.MipLevels = 1;
  m_device->CreateShaderResourceView(m_depthBuffer.Get(), &depthSrv,
                                     m_depthSrvCpu);
}

// ---- Fence + sync ----

void DxContext::CreateFence() {
  ThrowIfFailed(
      m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
      "CreateFence failed");
  m_fenceValue = 0;
  for (auto &v : m_frameFenceValues)
    v = 0;
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent)
    throw std::runtime_error("CreateEventW failed");
}

void DxContext::WaitForGpu() {
  if (!m_queue || !m_fence)
    return;

  const uint64_t fenceToWait = ++m_fenceValue;
  ThrowIfFailed(m_queue->Signal(m_fence.Get(), fenceToWait),
                "Queue Signal failed");

  if (m_fence->GetCompletedValue() < fenceToWait) {
    ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWait, m_fenceEvent),
                  "SetEventOnCompletion failed");
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
}

void DxContext::MoveToNextFrame() {
  const uint64_t signalValue = ++m_fenceValue;
  ThrowIfFailed(m_queue->Signal(m_fence.Get(), signalValue),
                "Queue Signal failed");
  m_frameFenceValues[m_frameIndex] = signalValue;

  ThrowIfFailed(m_swapchain->Present(1, 0), "Present failed");
  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

  const uint64_t fenceToWait = m_frameFenceValues[m_frameIndex];
  if (fenceToWait != 0 && m_fence->GetCompletedValue() < fenceToWait) {
    ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWait, m_fenceEvent),
                  "SetEventOnCompletion failed");
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
}

// ---- Per-frame helpers ----

ID3D12Resource *DxContext::CurrentBackBuffer() const {
  return m_backBuffers[m_frameIndex].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DxContext::CurrentRtv() const {
  D3D12_CPU_DESCRIPTOR_HANDLE h =
      m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += static_cast<SIZE_T>(m_frameIndex) *
           static_cast<SIZE_T>(m_rtvDescriptorSize);
  return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE DxContext::Dsv() const {
  return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_VIRTUAL_ADDRESS DxContext::AllocFrameConstants(uint32_t sizeBytes,
                                                        void **outCpuPtr) {
  if (!outCpuPtr)
    throw std::runtime_error("AllocFrameConstants: outCpuPtr is null");

  auto &fr = m_frames[m_frameIndex];
  if (!fr.constants || !fr.constantsMapped)
    throw std::runtime_error("AllocFrameConstants: constants buffer not ready");

  const uint32_t aligned = Align256(sizeBytes);
  if (fr.constantsOffset + aligned > kFrameConstantsBytes)
    throw std::runtime_error("AllocFrameConstants: out of per-frame constants");

  *outCpuPtr = fr.constantsMapped + fr.constantsOffset;
  const D3D12_GPU_VIRTUAL_ADDRESS gpu =
      fr.constants->GetGPUVirtualAddress() + fr.constantsOffset;
  fr.constantsOffset += aligned;
  return gpu;
}

// ---- ImGui helpers ----

D3D12_CPU_DESCRIPTOR_HANDLE DxContext::ImGuiFontCpuHandle() const {
  return m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE DxContext::ImGuiFontGpuHandle() const {
  return m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
}

void DxContext::ImGuiAllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE *outCpu,
                              D3D12_GPU_DESCRIPTOR_HANDLE *outGpu) {
  if (!outCpu || !outGpu)
    throw std::runtime_error("ImGuiAllocSrv: null output handle");
  if (!m_imguiSrvHeap)
    throw std::runtime_error("ImGuiAllocSrv: SRV heap not created");
  if (m_imguiSrvNext >= m_imguiSrvCapacity)
    throw std::runtime_error("ImGuiAllocSrv: SRV heap out of descriptors");

  D3D12_CPU_DESCRIPTOR_HANDLE cpu =
      m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu =
      m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(m_imguiSrvNext) *
             static_cast<SIZE_T>(m_imguiSrvDescriptorSize);
  gpu.ptr += static_cast<UINT64>(m_imguiSrvNext) *
             static_cast<UINT64>(m_imguiSrvDescriptorSize);

  ++m_imguiSrvNext;
  *outCpu = cpu;
  *outGpu = gpu;
}

void DxContext::RequestPreviewTexture(const LoadedImage &img) {
  if (img.pixels.empty() || img.width <= 0 || img.height <= 0)
    return;
  // Store a copy; the actual GPU upload happens in BeginFrame.
  m_pendingPreview = img;
}

// ---- Post-process resources ----

void DxContext::CreatePostProcessResources() {
  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  // ---- Allocate SRV CPU handles once (reuse on resize) ----
  if (!m_postProcessSrvsAllocated) {
    m_hdrSrvCpu = AllocMainSrvCpu(1);
    m_hdrSrvGpu = MainSrvGpuFromCpu(m_hdrSrvCpu);
    m_ldrSrvCpu = AllocMainSrvCpu(1);
    m_ldrSrvGpu = MainSrvGpuFromCpu(m_ldrSrvCpu);
    for (uint32_t i = 0; i < kBloomMips; ++i) {
      m_bloomSrvCpu[i] = AllocMainSrvCpu(1);
      m_bloomMips[i].srvGpu = MainSrvGpuFromCpu(m_bloomSrvCpu[i]);
    }
    m_postProcessSrvsAllocated = true;
  }

  // ---- HDR target (full resolution) ----
  m_hdrTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_hdrFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_hdrFormat;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_hdrTarget)),
                  "CreateCommittedResource (HDR target) failed");
  }

  // HDR RTV (slot FrameCount in RTV heap)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(FrameCount) *
                  static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_hdrRtv = handle;
    m_device->CreateRenderTargetView(m_hdrTarget.Get(), nullptr, m_hdrRtv);
  }

  // HDR SRV
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_hdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_hdrTarget.Get(), &srv, m_hdrSrvCpu);
  }

  // ---- LDR target (full resolution, R8G8B8A8_UNORM) ----
  m_ldrTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_backBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_backBufferFormat;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_ldrTarget)),
                  "CreateCommittedResource (LDR target) failed");
  }

  // LDR RTV (slot FrameCount + 1)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(FrameCount + 1) *
                  static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_ldrRtv = handle;
    m_device->CreateRenderTargetView(m_ldrTarget.Get(), nullptr, m_ldrRtv);
  }

  // LDR SRV
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_backBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_ldrTarget.Get(), &srv, m_ldrSrvCpu);
  }

  // ---- Bloom mip chain (5 levels, each half the previous) ----
  uint32_t bw = m_width / 2;
  uint32_t bh = m_height / 2;
  for (uint32_t i = 0; i < kBloomMips; ++i) {
    bw = (bw < 1) ? 1 : bw;
    bh = (bh < 1) ? 1 : bh;

    m_bloomMips[i].tex.Reset();
    m_bloomMips[i].width = bw;
    m_bloomMips[i].height = bh;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = bw;
    desc.Height = bh;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_hdrFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_hdrFormat;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_bloomMips[i].tex)),
                  "CreateCommittedResource (Bloom mip) failed");

    // RTV (slot FrameCount + 2 + i)
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(FrameCount + 2 + i) *
                     static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_bloomMips[i].rtv = rtvHandle;
    m_device->CreateRenderTargetView(m_bloomMips[i].tex.Get(), nullptr,
                                     rtvHandle);

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_hdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_bloomMips[i].tex.Get(), &srv,
                                       m_bloomSrvCpu[i]);

    bw /= 2;
    bh /= 2;
  }

  // ---- SSAO resources (Phase 10.3) ----
  // Allocate SRV handles once.
  if (!m_ssaoSrvsAllocated) {
    m_viewNormalSrvCpu = AllocMainSrvCpu(1);
    m_viewNormalSrvGpu = MainSrvGpuFromCpu(m_viewNormalSrvCpu);
    m_ssaoSrvCpu = AllocMainSrvCpu(1);
    m_ssaoSrvGpu = MainSrvGpuFromCpu(m_ssaoSrvCpu);
    m_ssaoBlurSrvCpu = AllocMainSrvCpu(1);
    m_ssaoBlurSrvGpu = MainSrvGpuFromCpu(m_ssaoBlurSrvCpu);
    m_ssaoSrvsAllocated = true;
  }

  m_ssaoWidth = (m_width > 1) ? m_width / 2 : 1;
  m_ssaoHeight = (m_height > 1) ? m_height / 2 : 1;

  // RTV slot base for SSAO resources: FrameCount + 2 + kBloomMips
  const uint32_t ssaoRtvBase = FrameCount + 2 + kBloomMips;

  // View-space normal target (full resolution, R16G16B16A16_FLOAT).
  m_viewNormalTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // Default: packed up-facing normal (0.5, 0.5, 1.0) so sky pixels are safe.
    clear.Color[0] = 0.5f;
    clear.Color[1] = 0.5f;
    clear.Color[2] = 1.0f;
    clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_viewNormalTarget)),
                  "CreateCommittedResource (ViewNormal target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(ssaoRtvBase) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_viewNormalRtv = rtvH;
    m_device->CreateRenderTargetView(m_viewNormalTarget.Get(), nullptr,
                                     m_viewNormalRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_viewNormalTarget.Get(), &srv,
                                       m_viewNormalSrvCpu);
  }

  // SSAO target (half resolution, R8_UNORM).
  m_ssaoTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_ssaoWidth;
    desc.Height = m_ssaoHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8_UNORM;
    clear.Color[0] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_ssaoTarget)),
                  "CreateCommittedResource (SSAO target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(ssaoRtvBase + 1) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_ssaoRtv = rtvH;
    m_device->CreateRenderTargetView(m_ssaoTarget.Get(), nullptr, m_ssaoRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_ssaoTarget.Get(), &srv, m_ssaoSrvCpu);
  }

  // SSAO blur target (same size/format as SSAO target).
  m_ssaoBlurTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_ssaoWidth;
    desc.Height = m_ssaoHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8_UNORM;
    clear.Color[0] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_ssaoBlurTarget)),
                  "CreateCommittedResource (SSAO blur target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(ssaoRtvBase + 2) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_ssaoBlurRtv = rtvH;
    m_device->CreateRenderTargetView(m_ssaoBlurTarget.Get(), nullptr,
                                     m_ssaoBlurRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_ssaoBlurTarget.Get(), &srv,
                                       m_ssaoBlurSrvCpu);
  }

  // ---- Motion blur resources (Phase 10.5) ----
  // RTV slot base: after SSAO resources (ssaoRtvBase + 3)
  const uint32_t mbRtvBase = ssaoRtvBase + 3;

  // Allocate SRV handles once.
  if (!m_motionBlurSrvsAllocated) {
    m_velocitySrvCpu = AllocMainSrvCpu(1);
    m_velocitySrvGpu = MainSrvGpuFromCpu(m_velocitySrvCpu);
    m_ldr2SrvCpu = AllocMainSrvCpu(1);
    m_ldr2SrvGpu = MainSrvGpuFromCpu(m_ldr2SrvCpu);
    m_motionBlurSrvsAllocated = true;
  }

  // Velocity buffer (full resolution, R16G16_FLOAT).
  m_velocityTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R16G16_FLOAT;
    clear.Color[0] = clear.Color[1] = 0.0f;
    clear.Color[2] = clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_velocityTarget)),
                  "CreateCommittedResource (Velocity target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(mbRtvBase) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_velocityRtv = rtvH;
    m_device->CreateRenderTargetView(m_velocityTarget.Get(), nullptr,
                                     m_velocityRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_velocityTarget.Get(), &srv,
                                       m_velocitySrvCpu);
  }

  // LDR2 target (full resolution, same format as LDR — motion blur output).
  m_ldr2Target.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_backBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_backBufferFormat;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_ldr2Target)),
                  "CreateCommittedResource (LDR2 target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(mbRtvBase + 1) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_ldr2Rtv = rtvH;
    m_device->CreateRenderTargetView(m_ldr2Target.Get(), nullptr, m_ldr2Rtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_backBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_ldr2Target.Get(), &srv, m_ldr2SrvCpu);
  }

  // ---- DOF resources (Phase 10.6) ----
  // RTV slot: after motion blur resources (mbRtvBase + 2)
  const uint32_t dofRtvBase = mbRtvBase + 2;

  // Allocate SRV handle once.
  if (!m_dofSrvsAllocated) {
    m_dofSrvCpu = AllocMainSrvCpu(1);
    m_dofSrvGpu = MainSrvGpuFromCpu(m_dofSrvCpu);
    m_dofSrvsAllocated = true;
  }

  // DOF target (full resolution, same format as LDR).
  m_dofTarget.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_backBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_backBufferFormat;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_dofTarget)),
                  "CreateCommittedResource (DOF target) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(dofRtvBase) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_dofRtv = rtvH;
    m_device->CreateRenderTargetView(m_dofTarget.Get(), nullptr, m_dofRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_backBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_dofTarget.Get(), &srv, m_dofSrvCpu);
  }

  // ---- G-buffer resources (Phase 12.1 — Deferred Rendering) ----
  // Allocate SRV handles once: contiguous 5-descriptor block
  // (albedo, normal, material, emissive, depth copy) for deferred lighting table.
  if (!m_gbufferSrvsAllocated) {
    m_gbufferAlbedoSrvCpu = AllocMainSrvCpu(5); // contiguous block of 5
    m_gbufferAlbedoSrvGpu = MainSrvGpuFromCpu(m_gbufferAlbedoSrvCpu);

    const auto srvInc = static_cast<SIZE_T>(m_mainSrvDescriptorSize);
    m_gbufferNormalSrvCpu.ptr = m_gbufferAlbedoSrvCpu.ptr + srvInc;
    m_gbufferNormalSrvGpu.ptr = m_gbufferAlbedoSrvGpu.ptr + srvInc;
    m_gbufferMaterialSrvCpu.ptr = m_gbufferAlbedoSrvCpu.ptr + srvInc * 2;
    m_gbufferMaterialSrvGpu.ptr = m_gbufferAlbedoSrvGpu.ptr + srvInc * 2;
    m_gbufferEmissiveSrvCpu.ptr = m_gbufferAlbedoSrvCpu.ptr + srvInc * 3;
    m_gbufferEmissiveSrvGpu.ptr = m_gbufferAlbedoSrvGpu.ptr + srvInc * 3;
    // Slot 4 (depth copy) will be filled below after resource creation.
    m_gbufferSrvsAllocated = true;
  }

  // RTV slot base: after DOF (dofRtvBase + 1)
  const uint32_t gbufferRtvBase = dofRtvBase + 1;

  // RT0: Albedo (R8G8B8A8_UNORM, sRGB SRV view for correct gamma)
  m_gbufferAlbedo.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[0] = 0.0f; clear.Color[1] = 0.0f;
    clear.Color[2] = 0.0f; clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_gbufferAlbedo)),
                  "CreateCommittedResource (G-buffer Albedo) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gbufferRtvBase) *
             static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_gbufferAlbedoRtv = h;
    m_device->CreateRenderTargetView(m_gbufferAlbedo.Get(), nullptr,
                                     m_gbufferAlbedoRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_gbufferAlbedo.Get(), &srv,
                                       m_gbufferAlbedoSrvCpu);
  }

  // RT1: World-space normals (R16G16B16A16_FLOAT)
  m_gbufferNormal.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clear.Color[0] = 0.0f; clear.Color[1] = 1.0f;
    clear.Color[2] = 0.0f; clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_gbufferNormal)),
                  "CreateCommittedResource (G-buffer Normal) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gbufferRtvBase + 1) *
             static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_gbufferNormalRtv = h;
    m_device->CreateRenderTargetView(m_gbufferNormal.Get(), nullptr,
                                     m_gbufferNormalRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_gbufferNormal.Get(), &srv,
                                       m_gbufferNormalSrvCpu);
  }

  // RT2: Material (R8G8B8A8_UNORM — R=metallic, G=roughness, B=AO)
  m_gbufferMaterial.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[0] = 0.0f; clear.Color[1] = 0.5f;
    clear.Color[2] = 1.0f; clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_gbufferMaterial)),
                  "CreateCommittedResource (G-buffer Material) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gbufferRtvBase + 2) *
             static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_gbufferMaterialRtv = h;
    m_device->CreateRenderTargetView(m_gbufferMaterial.Get(), nullptr,
                                     m_gbufferMaterialRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_gbufferMaterial.Get(), &srv,
                                       m_gbufferMaterialSrvCpu);
  }

  // RT3: Emissive (R11G11B10_FLOAT — HDR)
  m_gbufferEmissive.Reset();
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    clear.Color[0] = 0.0f; clear.Color[1] = 0.0f;
    clear.Color[2] = 0.0f; clear.Color[3] = 0.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_gbufferEmissive)),
                  "CreateCommittedResource (G-buffer Emissive) failed");
  }
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gbufferRtvBase + 3) *
             static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_gbufferEmissiveRtv = h;
    m_device->CreateRenderTargetView(m_gbufferEmissive.Get(), nullptr,
                                     m_gbufferEmissiveRtv);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_gbufferEmissive.Get(), &srv,
                                       m_gbufferEmissiveSrvCpu);
  }

  // Create depth SRV in 5th slot of G-buffer SRV table (for deferred lighting).
  {
    D3D12_CPU_DESCRIPTOR_HANDLE depthSlot;
    depthSlot.ptr = m_gbufferAlbedoSrvCpu.ptr +
                    static_cast<SIZE_T>(m_mainSrvDescriptorSize) * 4;
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_depthBuffer.Get(), &depthSrv,
                                       depthSlot);
  }

  // ---- TAA resources (Phase 10.4) ----
  // RTV slot base: after G-buffer (gbufferRtvBase + 4)
  const uint32_t taaRtvBase = gbufferRtvBase + 4;

  // Allocate SRV handles once.
  if (!m_taaSrvsAllocated) {
    for (int i = 0; i < 2; ++i) {
      m_taaHistorySrvCpu[i] = AllocMainSrvCpu(1);
      m_taaHistorySrvGpu[i] = MainSrvGpuFromCpu(m_taaHistorySrvCpu[i]);
    }
    m_taaSrvsAllocated = true;
  }

  for (int i = 0; i < 2; ++i) {
    m_taaHistory[i].Reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_hdrFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_hdrFormat;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                      IID_PPV_ARGS(&m_taaHistory[i])),
                  "CreateCommittedResource (TAA history) failed");

    // RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += static_cast<SIZE_T>(taaRtvBase + i) *
                static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_taaHistoryRtv[i] = rtvH;
    m_device->CreateRenderTargetView(m_taaHistory[i].Get(), nullptr, rtvH);

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = m_hdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_taaHistory[i].Get(), &srv,
                                       m_taaHistorySrvCpu[i]);
  }

  m_taaCurrentIndex = 0;
  m_taaFirstFrame = true;
}

// ---- Resize ----

void DxContext::Resize(uint32_t width, uint32_t height) {
  if (!m_swapchain)
    return;
  if (width == 0 || height == 0)
    return;

  m_width = width;
  m_height = height;

  WaitForGpu();

  for (auto &bb : m_backBuffers)
    bb.Reset();

  ThrowIfFailed(m_swapchain->ResizeBuffers(FrameCount, m_width, m_height,
                                           m_backBufferFormat, 0),
                "ResizeBuffers failed");

  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
  RecreateSizeDependentResources();
  CreateDepthResources();
  CreatePostProcessResources();
}

// ---- Frame begin / end ----

void DxContext::BeginFrame() {
  const uint64_t fenceToWait = m_frameFenceValues[m_frameIndex];
  if (fenceToWait != 0 && m_fence->GetCompletedValue() < fenceToWait) {
    ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWait, m_fenceEvent),
                  "SetEventOnCompletion failed");
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }

  ThrowIfFailed(m_frames[m_frameIndex].cmdAlloc->Reset(),
                "CommandAllocator Reset failed");
  ThrowIfFailed(
      m_cmdList->Reset(m_frames[m_frameIndex].cmdAlloc.Get(), nullptr),
      "CommandList Reset failed");

  // Reset per-frame constants ring.
  m_frames[m_frameIndex].constantsOffset = 0;

  // ---- Process pending preview texture upload (Phase 7 Asset Browser) ----
  if (!m_pendingPreview.pixels.empty()) {
    if (!m_previewSrvAllocated) {
      ImGuiAllocSrv(&m_previewSrvCpu, &m_previewSrvGpu);
      m_previewSrvAllocated = true;
    }

    const auto &img = m_pendingPreview;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = static_cast<UINT64>(img.width);
    texDesc.Height = static_cast<UINT>(img.height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    m_previewTex.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_previewTex)),
        "Preview: CreateCommittedResource failed");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 uploadBufferSize = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                                    &rowSizeBytes, &uploadBufferSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadBufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    m_previewUpload.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_previewUpload)),
        "Preview: upload buffer failed");

    void *mapped = nullptr;
    m_previewUpload->Map(0, nullptr, &mapped);
    auto *dest = reinterpret_cast<uint8_t *>(mapped);
    const uint8_t *srcData = img.pixels.data();
    for (UINT y = 0; y < numRows; ++y) {
      memcpy(dest + footprint.Offset + y * footprint.Footprint.RowPitch,
             srcData + y * (img.width * 4),
             static_cast<size_t>(img.width * 4));
    }
    m_previewUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = m_previewUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = m_previewTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    Transition(m_previewTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_previewTex.Get(), &srvDesc,
                                       m_previewSrvCpu);

    m_pendingPreview.pixels.clear(); // consumed
  }

  // Transition backbuffer to render target.
  Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
             D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Transition HDR target to render target for scene passes.
  Transition(m_hdrTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Transition view-normal target to render target for MRT opaque pass.
  if (m_viewNormalTarget)
    Transition(m_viewNormalTarget.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Transition G-buffer targets to render target (Phase 12.1).
  if (m_gbufferAlbedo)
    Transition(m_gbufferAlbedo.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (m_gbufferNormal)
    Transition(m_gbufferNormal.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (m_gbufferMaterial)
    Transition(m_gbufferMaterial.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (m_gbufferEmissive)
    Transition(m_gbufferEmissive.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void DxContext::Clear(float r, float g, float b, float a) {
  const float color[4] = {r, g, b, a};
  auto rtv = m_hdrRtv;
  auto dsv = Dsv();
  m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  m_cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void DxContext::ClearDepth(float depth) {
  m_cmdList->ClearDepthStencilView(Dsv(), D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0,
                                   nullptr);
}

void DxContext::EndFrame() {
  // Transition backbuffer to present.
  Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_PRESENT);

  ThrowIfFailed(m_cmdList->Close(), "CommandList Close failed");

  ID3D12CommandList *lists[] = {m_cmdList.Get()};
  m_queue->ExecuteCommandLists(1, lists);

  MoveToNextFrame();
}
