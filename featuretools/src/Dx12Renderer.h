#pragma once

#include "FeatureRegistry.h"
#include "FeatureWorld.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class Dx12Renderer {
public:
  Dx12Renderer() = default;
  ~Dx12Renderer();

  Dx12Renderer(const Dx12Renderer &) = delete;
  Dx12Renderer &operator=(const Dx12Renderer &) = delete;

  bool Initialize(HWND viewportWindow, uint32_t width, uint32_t height,
                  const std::filesystem::path &shaderPath,
                  std::wstring &error);
  void Shutdown();
  bool Resize(uint32_t width, uint32_t height, std::wstring &error);
  bool Render(const FeatureWorld &world, const FeatureRegistry &features,
              float deltaSeconds, std::wstring &error);

  void Orbit(float deltaYaw, float deltaPitch);
  void Zoom(float wheelDelta);
  void ResetCamera();

  [[nodiscard]] float LastGpuFrameMilliseconds() const noexcept {
    return m_lastFrameMilliseconds;
  }
  [[nodiscard]] uint64_t DrawCalls() const noexcept { return m_drawCalls; }
  [[nodiscard]] float CameraYaw() const noexcept { return m_yaw; }
  [[nodiscard]] std::wstring AdapterName() const { return m_adapterName; }

public:
  struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
  };

private:

  struct Mesh {
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    uint32_t indexCount = 0;
  };

  struct Texture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  struct alignas(16) PointLightConstants {
    DirectX::XMFLOAT4 positionRange{};
    DirectX::XMFLOAT4 colorIntensity{};
  };

  struct FrameConstants {
    DirectX::XMFLOAT4X4 viewProj{};
    DirectX::XMFLOAT4X4 lightViewProj{};
    DirectX::XMFLOAT4 cameraPositionTime{};
    DirectX::XMFLOAT4 cameraRightViewportX{};
    DirectX::XMFLOAT4 cameraUpViewportY{};
    DirectX::XMFLOAT4 sunDirectionIntensity{};
    DirectX::XMFLOAT4 sunColor{};
    std::array<PointLightConstants, 8> pointLights{};
    uint32_t pointLightCount = 0;
    uint32_t featureFlags = 0;
    uint32_t attackType = 0;
    uint32_t phaseTwo = 0;
  };

  struct ObjectConstants {
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMFLOAT4 baseColor{};
    DirectX::XMFLOAT4 materialParams{};
    uint32_t entityFlags = 0;
    uint32_t objectType = 0;
    uint32_t instanceCount = 1;
    uint32_t padding = 0;
  };

  struct UploadAllocation {
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    void *cpu = nullptr;
  };

  bool CreateDeviceAndSwapChain(HWND window, std::wstring &error);
  bool CreateDescriptorHeaps(std::wstring &error);
  bool CreateRootSignature(std::wstring &error);
  bool CreatePipelineStates(const std::filesystem::path &shaderPath,
                            std::wstring &error);
  bool CreateMeshes(std::wstring &error);
  bool CreateUploadArena(std::wstring &error);
  bool CreateSizeDependentResources(std::wstring &error);
  void ReleaseSizeDependentResources();
  void WaitForGpu();
  void WaitForCurrentFrame();
  bool CheckDebugMessages(std::wstring &error);

  UploadAllocation AllocateConstants(const void *data, size_t bytes);
  void Transition(Texture &texture, D3D12_RESOURCE_STATES nextState);
  void DrawEntities(const FeatureWorld &world, const FeatureRegistry &features,
                    D3D12_GPU_VIRTUAL_ADDRESS frameConstants,
                    bool transparent, bool shadowPass);
  void DrawFullscreen(ID3D12PipelineState *pipeline,
                      D3D12_GPU_VIRTUAL_ADDRESS frameConstants,
                      D3D12_CPU_DESCRIPTOR_HANDLE target,
                      DXGI_FORMAT format);
  bool IsEntityVisible(const LabEntity &entity,
                       const FeatureRegistry &features) const;
  uint32_t BuildFeatureFlags(const FeatureRegistry &features) const;
  FrameConstants BuildFrameConstants(const FeatureWorld &world,
                                     const FeatureRegistry &features,
                                     float deltaSeconds);
  uint32_t ObjectType(const LabEntity &entity) const;

  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const;
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv(uint32_t index) const;
  D3D12_GPU_DESCRIPTOR_HANDLE SrvTable() const;

  HWND m_window = nullptr;
  uint32_t m_width = 1;
  uint32_t m_height = 1;
  static constexpr uint32_t kFrameCount = 2;

  Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> m_backBuffers;
  std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount>
      m_allocators;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
#ifdef _DEBUG
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
  uint64_t m_debugMessageCursor = 0;
#endif
  std::array<uint64_t, kFrameCount> m_frameFenceValues{};
  uint64_t m_nextFenceValue = 1;
  HANDLE m_fenceEvent = nullptr;
  uint32_t m_frameIndex = 0;

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
  uint32_t m_rtvStride = 0;
  uint32_t m_dsvStride = 0;
  uint32_t m_srvStride = 0;

  Texture m_gbufferAlbedo;
  Texture m_gbufferNormal;
  Texture m_gbufferPosition;
  Texture m_shadow;
  Texture m_ao;
  Texture m_scene;
  Texture m_composite;
  Texture m_bloomA;
  Texture m_bloomB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gbufferPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gbufferWirePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_forwardPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_forwardWirePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_transparentPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ssaoPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_copyPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomExtractPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomBlurHPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomBlurVPso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_tonemapPso;

  std::unordered_map<LabMeshKind, Mesh> m_meshes;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadArena;
  uint8_t *m_uploadMapped = nullptr;
  size_t m_uploadOffset = 0;
  static constexpr size_t kUploadArenaSize = 4u * 1024u * 1024u;

  float m_yaw = 0.0f;
  float m_pitch = 0.24f;
  float m_distance = 31.0f;
  float m_elapsedTime = 0.0f;
  float m_lastFrameMilliseconds = 0.0f;
  uint64_t m_drawCalls = 0;
  std::wstring m_adapterName;
};
