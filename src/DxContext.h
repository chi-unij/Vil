// ======================================
// File: DxContext.h
// Purpose: DirectX 12 device context — core device/queue/swapchain + frame
//          management + descriptor heaps. Phase 8: trimmed to device manager
//          only; rendering logic lives in renderer modules and render passes.
// ======================================

#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>

#include "GltfLoader.h"
#include "Lighting.h"
#include "ShadowMap.h"
#include <DirectXMath.h>
#include <wrl.h>

class MeshRenderer;
class ParticleRenderer;
class PostProcessRenderer;
class Emitter;

class DxContext {
public:
  static constexpr uint32_t FrameCount = 2;
  static constexpr uint32_t kBloomMips = 5;

  DxContext();
  ~DxContext();

  void Initialize(HWND hwnd, uint32_t width, uint32_t height,
                  bool enableDebugLayer);
  void Shutdown();

  void Resize(uint32_t width, uint32_t height);

  // ---- Frame management ----
  void BeginFrame();
  void EndFrame();

  // ---- Render helpers ----
  void Clear(float r, float g, float b, float a);
  void ClearDepth(float depth);
  void Transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after);
  void SetViewportScissorFull();

  // ---- Resource allocation ----
  D3D12_GPU_VIRTUAL_ADDRESS AllocFrameConstants(uint32_t sizeBytes,
                                                void **outCpuPtr);
  D3D12_CPU_DESCRIPTOR_HANDLE AllocMainSrvCpu(uint32_t count = 1);
  D3D12_GPU_DESCRIPTOR_HANDLE
  MainSrvGpuFromCpu(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
  void WaitForGpu();

  // ---- Convenience wrappers (init-time) ----
  uint32_t CreateMeshResources(const LoadedMesh &mesh,
                               const MaterialImages &images = {},
                               const Material &material = {});
  void InitParticleRenderer();

  // Texture replacement (editor material editor).
  void ReplaceMeshTexture(uint32_t meshId, uint32_t slot,
                          const LoadedImage &img);
  void ClearMeshTexture(uint32_t meshId, uint32_t slot);
  D3D12_GPU_DESCRIPTOR_HANDLE GetTextureImGuiSrv(uint32_t meshId,
                                                  uint32_t slot);

  // ---- Accessors ----
  ID3D12Device *Device() const { return m_device.Get(); }
  ID3D12GraphicsCommandList *CmdList() const { return m_cmdList.Get(); }
  ID3D12CommandQueue *Queue() const { return m_queue.Get(); }
  ID3D12DescriptorHeap *MainSrvHeap() const { return m_mainSrvHeap.Get(); }
  DXGI_FORMAT BackBufferFormat() const { return m_backBufferFormat; }
  DXGI_FORMAT DepthFormat() const { return m_depthFormat; }
  uint32_t Width() const { return m_width; }
  uint32_t Height() const { return m_height; }
  uint32_t FrameIndex() const { return m_frameIndex; }

  D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const;
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const;

  // ---- Depth SRV (Phase 10.3) ----
  D3D12_GPU_DESCRIPTOR_HANDLE DepthSrvGpu() const { return m_depthSrvGpu; }
  ID3D12Resource *DepthBuffer() const { return m_depthBuffer.Get(); }

  // ---- G-buffer resource accessors (Phase 12.1 — Deferred Rendering) ----
  D3D12_CPU_DESCRIPTOR_HANDLE GBufferAlbedoRtv() const { return m_gbufferAlbedoRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE GBufferAlbedoSrvGpu() const { return m_gbufferAlbedoSrvGpu; }
  ID3D12Resource *GBufferAlbedoTarget() const { return m_gbufferAlbedo.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE GBufferNormalRtv() const { return m_gbufferNormalRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE GBufferNormalSrvGpu() const { return m_gbufferNormalSrvGpu; }
  ID3D12Resource *GBufferNormalTarget() const { return m_gbufferNormal.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE GBufferMaterialRtv() const { return m_gbufferMaterialRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE GBufferMaterialSrvGpu() const { return m_gbufferMaterialSrvGpu; }
  ID3D12Resource *GBufferMaterialTarget() const { return m_gbufferMaterial.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE GBufferEmissiveRtv() const { return m_gbufferEmissiveRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE GBufferEmissiveSrvGpu() const { return m_gbufferEmissiveSrvGpu; }
  ID3D12Resource *GBufferEmissiveTarget() const { return m_gbufferEmissive.Get(); }

  // G-buffer SRV table: contiguous 5-descriptor block (albedo, normal, material, emissive, depth)
  D3D12_GPU_DESCRIPTOR_HANDLE GBufferTableSrvGpu() const { return m_gbufferAlbedoSrvGpu; }

  // IBL table (shared with deferred lighting pass)
  D3D12_GPU_DESCRIPTOR_HANDLE IblTableGpu() const { return m_iblTableGpu; }
  void SetIblTableGpu(D3D12_GPU_DESCRIPTOR_HANDLE h) { m_iblTableGpu = h; }

  // ---- Legacy SSAO resource accessors (Phase 10.3) ----
  D3D12_CPU_DESCRIPTOR_HANDLE ViewNormalRtv() const { return m_viewNormalRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE ViewNormalSrvGpu() const { return m_viewNormalSrvGpu; }
  ID3D12Resource *ViewNormalTarget() const { return m_viewNormalTarget.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE SsaoRtv() const { return m_ssaoRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE SsaoSrvGpu() const { return m_ssaoSrvGpu; }
  ID3D12Resource *SsaoTarget() const { return m_ssaoTarget.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE SsaoBlurRtv() const { return m_ssaoBlurRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE SsaoBlurSrvGpu() const { return m_ssaoBlurSrvGpu; }
  ID3D12Resource *SsaoBlurTarget() const { return m_ssaoBlurTarget.Get(); }

  uint32_t SsaoWidth() const { return m_ssaoWidth; }
  uint32_t SsaoHeight() const { return m_ssaoHeight; }

  // ---- Post-process resource accessors ----
  DXGI_FORMAT HdrFormat() const { return m_hdrFormat; }
  D3D12_CPU_DESCRIPTOR_HANDLE HdrRtv() const { return m_hdrRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE HdrSrvGpu() const { return m_hdrSrvGpu; }
  ID3D12Resource *HdrTarget() const { return m_hdrTarget.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE LdrRtv() const { return m_ldrRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE LdrSrvGpu() const { return m_ldrSrvGpu; }
  ID3D12Resource *LdrTarget() const { return m_ldrTarget.Get(); }

  // ---- Motion blur resource accessors (Phase 10.5) ----
  D3D12_CPU_DESCRIPTOR_HANDLE VelocityRtv() const { return m_velocityRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE VelocitySrvGpu() const { return m_velocitySrvGpu; }
  ID3D12Resource *VelocityTarget() const { return m_velocityTarget.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE Ldr2Rtv() const { return m_ldr2Rtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE Ldr2SrvGpu() const { return m_ldr2SrvGpu; }
  ID3D12Resource *Ldr2Target() const { return m_ldr2Target.Get(); }

  // ---- TAA resource accessors (Phase 10.4) ----
  D3D12_CPU_DESCRIPTOR_HANDLE TaaOutputRtv() const { return m_taaHistoryRtv[m_taaCurrentIndex]; }
  D3D12_GPU_DESCRIPTOR_HANDLE TaaHistorySrvGpu() const { return m_taaHistorySrvGpu[1 - m_taaCurrentIndex]; }
  D3D12_GPU_DESCRIPTOR_HANDLE TaaOutputSrvGpu() const { return m_taaHistorySrvGpu[m_taaCurrentIndex]; }
  ID3D12Resource *TaaOutputTarget() const { return m_taaHistory[m_taaCurrentIndex].Get(); }
  ID3D12Resource *TaaHistoryTarget() const { return m_taaHistory[1 - m_taaCurrentIndex].Get(); }
  void SwapTaaBuffers() { m_taaCurrentIndex = 1 - m_taaCurrentIndex; }
  bool TaaFirstFrame() const { return m_taaFirstFrame; }
  void ClearTaaFirstFrame() { m_taaFirstFrame = false; }
  void ResetTaaFirstFrame() { m_taaFirstFrame = true; }

  // ---- DOF resource accessors (Phase 10.6) ----
  D3D12_CPU_DESCRIPTOR_HANDLE DofRtv() const { return m_dofRtv; }
  D3D12_GPU_DESCRIPTOR_HANDLE DofSrvGpu() const { return m_dofSrvGpu; }
  ID3D12Resource *DofTarget() const { return m_dofTarget.Get(); }

  struct BloomMip {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    uint32_t width = 0, height = 0;
  };
  const BloomMip &GetBloomMip(uint32_t i) const { return m_bloomMips[i]; }

  // ---- Renderer access (passes call renderers directly) ----
  MeshRenderer &GetMeshRenderer();
  ShadowMap &GetShadowMap();
  ParticleRenderer &GetParticleRenderer();

  // ---- ImGui ----
  ID3D12DescriptorHeap *ImGuiSrvHeap() const { return m_imguiSrvHeap.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE ImGuiFontCpuHandle() const;
  D3D12_GPU_DESCRIPTOR_HANDLE ImGuiFontGpuHandle() const;
  void ImGuiAllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE *outCpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE *outGpu);

  // Queue a preview texture for upload (processed during next BeginFrame).
  // Returns the GPU handle that will be valid after the next BeginFrame.
  void RequestPreviewTexture(const LoadedImage &img);

  // Returns the current preview SRV GPU handle (0 if none loaded yet).
  D3D12_GPU_DESCRIPTOR_HANDLE PreviewTextureGpu() const { return m_previewSrvGpu; }
  bool HasPreviewTexture() const { return m_previewSrvAllocated && m_previewTex; }

private:
  friend class MeshRenderer;
  friend class ShadowMap;
  friend class ParticleRenderer;
  friend class SkyRenderer;
  friend class GridRenderer;
  friend class PostProcessRenderer;
  friend class IBLGenerator;
  friend class SSAORenderer;

  void CreateDeviceAndQueue(bool enableDebugLayer);
  void CreateSwapChain(HWND hwnd);
  void CreateRtvHeapAndViews();
  void CreateCommandObjects();
  void CreateFence();
  void MoveToNextFrame();
  void RecreateSizeDependentResources();
  void CreateDepthResources();
  void CreateImGuiResources();
  void CreateMainSrvHeap();
  void CreatePostProcessResources();

  ID3D12Resource *CurrentBackBuffer() const;

private:
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
  uint32_t m_frameIndex = 0;

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  uint32_t m_rtvDescriptorSize = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_backBuffers;

  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;

  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  uint64_t m_fenceValue = 0;
  std::array<uint64_t, FrameCount> m_frameFenceValues{};
  HANDLE m_fenceEvent = nullptr;

  bool m_enableDebugLayer = false;

  // Depth
  DXGI_FORMAT m_depthFormat = DXGI_FORMAT_D32_FLOAT;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
  D3D12_CPU_DESCRIPTOR_HANDLE m_depthSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_depthSrvGpu{};
  bool m_depthSrvAllocated = false;

  // Rendering modules (moved out of DxContext.cpp for maintainability).
  std::unique_ptr<MeshRenderer> m_meshRenderer;
  std::unique_ptr<ShadowMap> m_shadowMap;
  std::unique_ptr<ParticleRenderer> m_particleRenderer;

  // ImGui SRV heap (shader-visible, used for font texture)
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
  uint32_t m_imguiSrvDescriptorSize = 0;
  uint32_t m_imguiSrvCapacity = 0;
  uint32_t m_imguiSrvNext = 0;

  // Preview texture (Asset Browser thumbnail — reuses single descriptor slot).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_previewTex;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_previewUpload; // must stay alive until GPU finishes copy
  D3D12_CPU_DESCRIPTOR_HANDLE m_previewSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_previewSrvGpu{};
  bool m_previewSrvAllocated = false;
  LoadedImage m_pendingPreview; // set by RequestPreviewTexture, consumed by BeginFrame

  // Main SRV heap (shader-visible) for app resources
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_mainSrvHeap;
  uint32_t m_mainSrvDescriptorSize = 0;
  uint32_t m_mainSrvCapacity = 0;
  uint32_t m_mainSrvNext = 0;

  // ---- Post-process resources ----
  DXGI_FORMAT m_hdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_hdrTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_hdrRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_hdrSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_hdrSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldrTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_ldrRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_ldrSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_ldrSrvGpu{};

  std::array<BloomMip, kBloomMips> m_bloomMips;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBloomMips> m_bloomSrvCpu{};

  bool m_postProcessSrvsAllocated = false;

  // ---- G-buffer resources (Phase 12.1 — Deferred Rendering) ----
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gbufferAlbedo;
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferAlbedoRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferAlbedoSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_gbufferAlbedoSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gbufferNormal;
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferNormalRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferNormalSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_gbufferNormalSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gbufferMaterial;
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferMaterialRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferMaterialSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_gbufferMaterialSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gbufferEmissive;
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferEmissiveRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_gbufferEmissiveSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_gbufferEmissiveSrvGpu{};

  bool m_gbufferSrvsAllocated = false;

  D3D12_GPU_DESCRIPTOR_HANDLE m_iblTableGpu{}; // shared IBL table for deferred lighting

  // ---- SSAO resources (Phase 10.3) ----
  Microsoft::WRL::ComPtr<ID3D12Resource> m_viewNormalTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_viewNormalRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_viewNormalSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_viewNormalSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_ssaoTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_ssaoRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_ssaoSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_ssaoSrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_ssaoBlurTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_ssaoBlurRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_ssaoBlurSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_ssaoBlurSrvGpu{};

  bool m_ssaoSrvsAllocated = false;
  uint32_t m_ssaoWidth = 0, m_ssaoHeight = 0;

  // ---- Motion blur resources (Phase 10.5) ----
  Microsoft::WRL::ComPtr<ID3D12Resource> m_velocityTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_velocityRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_velocitySrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_velocitySrvGpu{};

  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldr2Target;
  D3D12_CPU_DESCRIPTOR_HANDLE m_ldr2Rtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_ldr2SrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_ldr2SrvGpu{};

  bool m_motionBlurSrvsAllocated = false;

  // ---- DOF resources (Phase 10.6) ----
  Microsoft::WRL::ComPtr<ID3D12Resource> m_dofTarget;
  D3D12_CPU_DESCRIPTOR_HANDLE m_dofRtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_dofSrvCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_dofSrvGpu{};
  bool m_dofSrvsAllocated = false;

  // ---- TAA resources (Phase 10.4) ----
  Microsoft::WRL::ComPtr<ID3D12Resource> m_taaHistory[2];
  D3D12_CPU_DESCRIPTOR_HANDLE m_taaHistoryRtv[2]{};
  D3D12_CPU_DESCRIPTOR_HANDLE m_taaHistorySrvCpu[2]{};
  D3D12_GPU_DESCRIPTOR_HANDLE m_taaHistorySrvGpu[2]{};
  bool m_taaSrvsAllocated = false;
  uint32_t m_taaCurrentIndex = 0;
  bool m_taaFirstFrame = true;

  struct FrameResources {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmdAlloc;

    // Per-frame constant buffer ring (upload heap) for per-draw constants.
    Microsoft::WRL::ComPtr<ID3D12Resource> constants;
    uint8_t *constantsMapped = nullptr;
    uint32_t constantsOffset = 0;
  };
  std::array<FrameResources, FrameCount> m_frames;
};
