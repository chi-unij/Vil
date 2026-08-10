#include "Dx12Renderer.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr uint32_t kShadowSize = 2048;

std::wstring HrMessage(HRESULT hr, const wchar_t *operation) {
  wchar_t *systemMessage = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(hr), 0,
                 reinterpret_cast<wchar_t *>(&systemMessage), 0, nullptr);
  std::wostringstream stream;
  stream << operation << L" failed (0x" << std::hex
         << static_cast<unsigned long>(hr) << L")";
  if (systemMessage) {
    stream << L": " << systemMessage;
    LocalFree(systemMessage);
  }
  return stream.str();
}

bool Check(HRESULT hr, const wchar_t *operation, std::wstring &error) {
  if (SUCCEEDED(hr))
    return true;
  error = HrMessage(hr, operation);
  return false;
}

size_t Align256(size_t value) { return (value + 255u) & ~size_t(255u); }

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES value{};
  value.Type = type;
  value.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  value.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  value.CreationNodeMask = 1;
  value.VisibleNodeMask = 1;
  return value;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t size) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

D3D12_RASTERIZER_DESC Rasterizer(bool wireframe = false) {
  D3D12_RASTERIZER_DESC value{};
  value.FillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  value.CullMode = D3D12_CULL_MODE_BACK;
  value.FrontCounterClockwise = FALSE;
  value.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
  value.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
  value.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
  value.DepthClipEnable = TRUE;
  return value;
}

D3D12_BLEND_DESC BlendDisabled() {
  D3D12_BLEND_DESC value{};
  for (auto &target : value.RenderTarget)
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  return value;
}

D3D12_DEPTH_STENCIL_DESC DepthState(bool enabled, bool write) {
  D3D12_DEPTH_STENCIL_DESC value{};
  value.DepthEnable = enabled;
  value.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL
                               : D3D12_DEPTH_WRITE_MASK_ZERO;
  value.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  value.StencilEnable = FALSE;
  return value;
}

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path &path,
                              const char *entry, const char *target,
                              std::wstring &error) {
  ComPtr<ID3DBlob> shader;
  ComPtr<ID3DBlob> diagnostics;
  const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
#ifdef _DEBUG
                     D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
                     D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr,
                                        D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        entry, target, flags, 0, &shader,
                                        &diagnostics);
  if (FAILED(hr)) {
    std::string detail;
    if (diagnostics)
      detail.assign(static_cast<const char *>(diagnostics->GetBufferPointer()),
                    diagnostics->GetBufferSize());
    error = L"Shader compile failed: " + path.wstring() + L" / " +
            std::wstring(entry, entry + std::strlen(entry)) + L"\n" +
            std::wstring(detail.begin(), detail.end());
    return {};
  }
  return shader;
}

std::vector<Dx12Renderer::Vertex> CubeVertices() {
  using V = Dx12Renderer::Vertex;
  return {
      {-1,-1,-1, 0,0,-1, 0,1}, {1,-1,-1, 0,0,-1, 1,1}, {1,1,-1, 0,0,-1, 1,0}, {-1,1,-1, 0,0,-1, 0,0},
      {1,-1,1, 0,0,1, 0,1}, {-1,-1,1, 0,0,1, 1,1}, {-1,1,1, 0,0,1, 1,0}, {1,1,1, 0,0,1, 0,0},
      {-1,-1,1, -1,0,0, 0,1}, {-1,-1,-1, -1,0,0, 1,1}, {-1,1,-1, -1,0,0, 1,0}, {-1,1,1, -1,0,0, 0,0},
      {1,-1,-1, 1,0,0, 0,1}, {1,-1,1, 1,0,0, 1,1}, {1,1,1, 1,0,0, 1,0}, {1,1,-1, 1,0,0, 0,0},
      {-1,1,-1, 0,1,0, 0,1}, {1,1,-1, 0,1,0, 1,1}, {1,1,1, 0,1,0, 1,0}, {-1,1,1, 0,1,0, 0,0},
      {-1,-1,1, 0,-1,0, 0,1}, {1,-1,1, 0,-1,0, 1,1}, {1,-1,-1, 0,-1,0, 1,0}, {-1,-1,-1, 0,-1,0, 0,0},
  };
}

std::vector<uint32_t> CubeIndices() {
  std::vector<uint32_t> result;
  for (uint32_t face = 0; face < 6; ++face) {
    const uint32_t base = face * 4;
    result.insert(result.end(), {base, base + 1, base + 2, base, base + 2,
                                 base + 3});
  }
  return result;
}

void MakeSphere(std::vector<Dx12Renderer::Vertex> &vertices,
                std::vector<uint32_t> &indices, uint32_t slices = 24,
                uint32_t stacks = 16) {
  for (uint32_t y = 0; y <= stacks; ++y) {
    const float v = static_cast<float>(y) / stacks;
    const float phi = v * std::numbers::pi_v<float>;
    for (uint32_t x = 0; x <= slices; ++x) {
      const float u = static_cast<float>(x) / slices;
      const float theta = u * std::numbers::pi_v<float> * 2.0f;
      const float sx = std::sin(phi) * std::cos(theta);
      const float sy = std::cos(phi);
      const float sz = std::sin(phi) * std::sin(theta);
      vertices.push_back({sx, sy, sz, sx, sy, sz, u, v});
    }
  }
  for (uint32_t y = 0; y < stacks; ++y) {
    for (uint32_t x = 0; x < slices; ++x) {
      const uint32_t a = y * (slices + 1) + x;
      const uint32_t b = a + slices + 1;
      indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
}

void MakeCylinder(std::vector<Dx12Renderer::Vertex> &vertices,
                  std::vector<uint32_t> &indices, uint32_t slices = 24) {
  for (uint32_t side = 0; side <= slices; ++side) {
    const float u = static_cast<float>(side) / slices;
    const float angle = u * std::numbers::pi_v<float> * 2.0f;
    const float x = std::cos(angle), z = std::sin(angle);
    vertices.push_back({x, -0.5f, z, x, 0, z, u, 1});
    vertices.push_back({x, 0.5f, z, x, 0, z, u, 0});
  }
  for (uint32_t side = 0; side < slices; ++side) {
    const uint32_t base = side * 2;
    indices.insert(indices.end(), {base, base + 1, base + 2, base + 2,
                                   base + 1, base + 3});
  }
  const uint32_t bottomCenter = static_cast<uint32_t>(vertices.size());
  vertices.push_back({0,-0.5f,0, 0,-1,0, 0.5f,0.5f});
  const uint32_t topCenter = static_cast<uint32_t>(vertices.size());
  vertices.push_back({0,0.5f,0, 0,1,0, 0.5f,0.5f});
  for (uint32_t side = 0; side < slices; ++side) {
    const uint32_t next = (side + 1) * 2;
    indices.insert(indices.end(), {bottomCenter, next, side * 2,
                                   topCenter, side * 2 + 1, next + 1});
  }
}

void MakeDisc(std::vector<Dx12Renderer::Vertex> &vertices,
              std::vector<uint32_t> &indices, bool ring) {
  constexpr uint32_t slices = 48;
  if (!ring) {
    vertices.push_back({0,0,0, 0,1,0, 0.5f,0.5f});
    for (uint32_t i = 0; i <= slices; ++i) {
      const float a = static_cast<float>(i) / slices *
                      std::numbers::pi_v<float> * 2.0f;
      vertices.push_back({std::cos(a),0,std::sin(a), 0,1,0,
                          std::cos(a)*0.5f+0.5f,std::sin(a)*0.5f+0.5f});
    }
    for (uint32_t i = 0; i < slices; ++i)
      indices.insert(indices.end(), {0, i + 1, i + 2});
    return;
  }
  for (uint32_t i = 0; i <= slices; ++i) {
    const float a = static_cast<float>(i) / slices *
                    std::numbers::pi_v<float> * 2.0f;
    const float x = std::cos(a), z = std::sin(a);
    vertices.push_back({x*0.72f,0,z*0.72f, 0,1,0, 0,0});
    vertices.push_back({x,0,z, 0,1,0, 1,1});
  }
  for (uint32_t i = 0; i < slices; ++i) {
    const uint32_t a = i * 2;
    indices.insert(indices.end(), {a, a + 1, a + 2, a + 2, a + 1,
                                   a + 3});
  }
}

} // namespace

Dx12Renderer::~Dx12Renderer() { Shutdown(); }

bool Dx12Renderer::Initialize(HWND viewportWindow, uint32_t width,
                              uint32_t height,
                              const std::filesystem::path &shaderPath,
                              std::wstring &error) {
  m_window = viewportWindow;
  m_width = std::max(1u, width);
  m_height = std::max(1u, height);
  if (!CreateDeviceAndSwapChain(viewportWindow, error) ||
      !CreateDescriptorHeaps(error) || !CreateRootSignature(error) ||
      !CreatePipelineStates(shaderPath, error) || !CreateMeshes(error) ||
      !CreateUploadArena(error) || !CreateSizeDependentResources(error)) {
    Shutdown();
    return false;
  }
  return true;
}

void Dx12Renderer::Shutdown() {
  if (m_device)
    WaitForGpu();
  if (m_uploadArena && m_uploadMapped)
    m_uploadArena->Unmap(0, nullptr);
  m_uploadMapped = nullptr;
  ReleaseSizeDependentResources();
  for (auto &buffer : m_backBuffers)
    buffer.Reset();
  m_meshes.clear();
  m_uploadArena.Reset();
  m_tonemapPso.Reset(); m_bloomBlurVPso.Reset(); m_bloomBlurHPso.Reset();
  m_bloomExtractPso.Reset(); m_copyPso.Reset(); m_deferredPso.Reset();
  m_ssaoPso.Reset(); m_transparentPso.Reset(); m_forwardWirePso.Reset();
  m_forwardPso.Reset(); m_gbufferWirePso.Reset(); m_gbufferPso.Reset();
  m_shadowPso.Reset(); m_rootSignature.Reset(); m_srvHeap.Reset();
  m_dsvHeap.Reset(); m_rtvHeap.Reset(); m_list.Reset();
  for (auto &allocator : m_allocators) allocator.Reset();
  m_swapChain.Reset(); m_queue.Reset(); m_fence.Reset(); m_device.Reset();
#ifdef _DEBUG
  m_infoQueue.Reset();
#endif
  m_adapter.Reset(); m_factory.Reset();
  if (m_fenceEvent) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
}

bool Dx12Renderer::CreateDeviceAndSwapChain(HWND window, std::wstring &error) {
#ifdef _DEBUG
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    debug->EnableDebugLayer();
#endif
  UINT factoryFlags = 0;
#ifdef _DEBUG
  factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
  HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
  if (FAILED(hr) && factoryFlags != 0)
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
  if (!Check(hr, L"CreateDXGIFactory2", error))
    return false;

  for (uint32_t index = 0;
       m_factory->EnumAdapterByGpuPreference(
           index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
           IID_PPV_ARGS(&m_adapter)) != DXGI_ERROR_NOT_FOUND;
       ++index) {
    DXGI_ADAPTER_DESC1 desc{};
    m_adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
        SUCCEEDED(D3D12CreateDevice(m_adapter.Get(),
                                    D3D_FEATURE_LEVEL_12_0,
                                    __uuidof(ID3D12Device), nullptr))) {
      m_adapterName = desc.Description;
      break;
    }
    m_adapter.Reset();
  }
  if (!m_adapter) {
    if (!Check(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&m_adapter)),
               L"EnumWarpAdapter", error))
      return false;
    m_adapterName = L"Microsoft WARP";
  }
  if (!Check(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                               IID_PPV_ARGS(&m_device)),
             L"D3D12CreateDevice", error))
    return false;
#ifdef _DEBUG
  if (SUCCEEDED(m_device.As(&m_infoQueue))) {
    m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    m_debugMessageCursor = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
  }
#endif

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (!Check(m_device->CreateCommandQueue(&queueDesc,
                                          IID_PPV_ARGS(&m_queue)),
             L"CreateCommandQueue", error))
    return false;
  for (auto &allocator : m_allocators) {
    if (!Check(m_device->CreateCommandAllocator(
                   D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               L"CreateCommandAllocator", error))
      return false;
  }
  if (!Check(m_device->CreateCommandList(
                 0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(),
                 nullptr, IID_PPV_ARGS(&m_list)),
             L"CreateCommandList", error) ||
      !Check(m_list->Close(), L"Close initial command list", error))
    return false;

  DXGI_SWAP_CHAIN_DESC1 swapDesc{};
  swapDesc.Width = m_width;
  swapDesc.Height = m_height;
  swapDesc.Format = kBackBufferFormat;
  swapDesc.SampleDesc.Count = 1;
  swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapDesc.BufferCount = kFrameCount;
  swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapChain1;
  if (!Check(m_factory->CreateSwapChainForHwnd(
                 m_queue.Get(), window, &swapDesc, nullptr, nullptr,
                 &swapChain1),
             L"CreateSwapChainForHwnd", error) ||
      !Check(swapChain1.As(&m_swapChain), L"Query IDXGISwapChain3", error))
    return false;
  m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  if (!Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&m_fence)),
             L"CreateFence", error))
    return false;
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent) {
    error = L"CreateEventW failed for the DX12 fence.";
    return false;
  }
  return true;
}

bool Dx12Renderer::CreateDescriptorHeaps(std::wstring &error) {
  D3D12_DESCRIPTOR_HEAP_DESC rtv{};
  rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv.NumDescriptors = 16;
  if (!Check(m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtvHeap)),
             L"Create RTV heap", error))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC dsv{};
  dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv.NumDescriptors = 2;
  if (!Check(m_device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&m_dsvHeap)),
             L"Create DSV heap", error))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC srv{};
  srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv.NumDescriptors = 9;
  srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (!Check(m_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_srvHeap)),
             L"Create SRV heap", error))
    return false;
  m_rtvStride = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_dsvStride = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  m_srvStride = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

bool Dx12Renderer::CreateRootSignature(std::wstring &error) {
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 9;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[1].Descriptor.ShaderRegister = 1;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &range;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samplers[2]{};
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].ShaderRegister = 0;
  samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[1] = samplers[0];
  samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[1].ShaderRegister = 1;

  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 3;
  desc.pParameters = params;
  desc.NumStaticSamplers = 2;
  desc.pStaticSamplers = samplers;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> signature;
  ComPtr<ID3DBlob> diagnostics;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &diagnostics);
  if (FAILED(hr)) {
    error = L"D3D12SerializeRootSignature failed.";
    if (diagnostics) {
      const char *text = static_cast<const char *>(diagnostics->GetBufferPointer());
      error += L" " + std::wstring(text, text + diagnostics->GetBufferSize());
    }
    return false;
  }
  return Check(m_device->CreateRootSignature(
                   0, signature->GetBufferPointer(), signature->GetBufferSize(),
                   IID_PPV_ARGS(&m_rootSignature)),
               L"CreateRootSignature", error);
}

bool Dx12Renderer::CreatePipelineStates(
    const std::filesystem::path &shaderPath, std::wstring &error) {
  auto geometryVs = CompileShader(shaderPath, "GeometryVS", "vs_5_1", error);
  auto shadowVs = CompileShader(shaderPath, "ShadowVS", "vs_5_1", error);
  auto fullscreenVs = CompileShader(shaderPath, "FullscreenVS", "vs_5_1", error);
  if (!geometryVs || !shadowVs || !fullscreenVs) return false;
  auto gbufferPs = CompileShader(shaderPath, "GBufferPS", "ps_5_1", error);
  auto forwardPs = CompileShader(shaderPath, "ForwardPS", "ps_5_1", error);
  auto transparentPs = CompileShader(shaderPath, "TransparentPS", "ps_5_1", error);
  auto ssaoPs = CompileShader(shaderPath, "SSAOPS", "ps_5_1", error);
  auto deferredPs = CompileShader(shaderPath, "DeferredLightingPS", "ps_5_1", error);
  auto copyPs = CompileShader(shaderPath, "CopyPS", "ps_5_1", error);
  auto bloomExtractPs = CompileShader(shaderPath, "BloomExtractPS", "ps_5_1", error);
  auto bloomHPs = CompileShader(shaderPath, "BloomBlurHPS", "ps_5_1", error);
  auto bloomVPs = CompileShader(shaderPath, "BloomBlurVPS", "ps_5_1", error);
  auto tonemapPs = CompileShader(shaderPath, "TonemapPS", "ps_5_1", error);
  if (!gbufferPs || !forwardPs || !transparentPs || !ssaoPs || !deferredPs ||
      !copyPs || !bloomExtractPs || !bloomHPs || !bloomVPs || !tonemapPs)
    return false;

  D3D12_INPUT_ELEMENT_DESC input[] = {
      {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
      {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
      {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  };
  auto base = [&]() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.InputLayout = {input, static_cast<UINT>(std::size(input))};
    pso.VS = {geometryVs->GetBufferPointer(), geometryVs->GetBufferSize()};
    pso.RasterizerState = Rasterizer();
    pso.BlendState = BlendDisabled();
    pso.DepthStencilState = DepthState(true, true);
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleDesc.Count = 1;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    return pso;
  };
  auto create = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                    ComPtr<ID3D12PipelineState> &out, const wchar_t *name) {
    return Check(m_device->CreateGraphicsPipelineState(&desc,
                                                        IID_PPV_ARGS(&out)),
                 name, error);
  };

  auto shadow = base();
  shadow.VS = {shadowVs->GetBufferPointer(), shadowVs->GetBufferSize()};
  shadow.NumRenderTargets = 0;
  shadow.RasterizerState.DepthBias = 900;
  shadow.RasterizerState.SlopeScaledDepthBias = 1.5f;
  if (!create(shadow, m_shadowPso, L"Create shadow PSO")) return false;

  auto gbuffer = base();
  gbuffer.PS = {gbufferPs->GetBufferPointer(), gbufferPs->GetBufferSize()};
  gbuffer.NumRenderTargets = 3;
  gbuffer.RTVFormats[0] = gbuffer.RTVFormats[1] = gbuffer.RTVFormats[2] = kHdrFormat;
  if (!create(gbuffer, m_gbufferPso, L"Create G-buffer PSO")) return false;
  auto gbufferWire = gbuffer;
  gbufferWire.RasterizerState = Rasterizer(true);
  gbufferWire.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  if (!create(gbufferWire, m_gbufferWirePso, L"Create wire G-buffer PSO")) return false;

  auto forward = base();
  forward.PS = {forwardPs->GetBufferPointer(), forwardPs->GetBufferSize()};
  forward.NumRenderTargets = 1;
  forward.RTVFormats[0] = kHdrFormat;
  if (!create(forward, m_forwardPso, L"Create forward PSO")) return false;
  auto forwardWire = forward;
  forwardWire.RasterizerState = Rasterizer(true);
  forwardWire.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  if (!create(forwardWire, m_forwardWirePso, L"Create wire forward PSO")) return false;

  auto transparent = forward;
  transparent.PS = {transparentPs->GetBufferPointer(), transparentPs->GetBufferSize()};
  transparent.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  transparent.DepthStencilState = DepthState(true, false);
  auto &blend = transparent.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (!create(transparent, m_transparentPso, L"Create transparent PSO")) return false;

  auto full = base();
  full.InputLayout = {nullptr, 0};
  full.VS = {fullscreenVs->GetBufferPointer(), fullscreenVs->GetBufferSize()};
  full.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  full.DepthStencilState = DepthState(false, false);
  full.DSVFormat = DXGI_FORMAT_UNKNOWN;
  full.NumRenderTargets = 1;
  full.RTVFormats[0] = kHdrFormat;
  auto makeFull = [&](ComPtr<ID3DBlob> ps, DXGI_FORMAT format,
                      ComPtr<ID3D12PipelineState> &out, const wchar_t *name) {
    auto desc = full;
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.RTVFormats[0] = format;
    return create(desc, out, name);
  };
  if (!makeFull(ssaoPs, DXGI_FORMAT_R8_UNORM, m_ssaoPso, L"Create SSAO PSO") ||
      !makeFull(deferredPs, kHdrFormat, m_deferredPso, L"Create deferred PSO") ||
      !makeFull(copyPs, kHdrFormat, m_copyPso, L"Create copy PSO") ||
      !makeFull(bloomExtractPs, kHdrFormat, m_bloomExtractPso, L"Create bloom extract PSO") ||
      !makeFull(bloomHPs, kHdrFormat, m_bloomBlurHPso, L"Create bloom H PSO") ||
      !makeFull(bloomVPs, kHdrFormat, m_bloomBlurVPso, L"Create bloom V PSO") ||
      !makeFull(tonemapPs, kBackBufferFormat, m_tonemapPso, L"Create tonemap PSO"))
    return false;
  return true;
}

bool Dx12Renderer::CreateMeshes(std::wstring &error) {
  auto createMesh = [&](LabMeshKind kind, const std::vector<Vertex> &vertices,
                        const std::vector<uint32_t> &indices) {
    Mesh mesh;
    const uint64_t vbSize = vertices.size() * sizeof(Vertex);
    const uint64_t ibSize = indices.size() * sizeof(uint32_t);
    auto upload = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc = BufferDesc(vbSize);
    if (!Check(m_device->CreateCommittedResource(
                   &upload, D3D12_HEAP_FLAG_NONE, &vbDesc,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(&mesh.vertexBuffer)),
               L"Create mesh vertex buffer", error)) return false;
    auto ibDesc = BufferDesc(ibSize);
    if (!Check(m_device->CreateCommittedResource(
                   &upload, D3D12_HEAP_FLAG_NONE, &ibDesc,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(&mesh.indexBuffer)),
               L"Create mesh index buffer", error)) return false;
    void *mapped = nullptr;
    mesh.vertexBuffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vbSize));
    mesh.vertexBuffer->Unmap(0, nullptr);
    mesh.indexBuffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, indices.data(), static_cast<size_t>(ibSize));
    mesh.indexBuffer->Unmap(0, nullptr);
    mesh.vertexView = {mesh.vertexBuffer->GetGPUVirtualAddress(),
                       static_cast<UINT>(vbSize), sizeof(Vertex)};
    mesh.indexView = {mesh.indexBuffer->GetGPUVirtualAddress(),
                      static_cast<UINT>(ibSize), DXGI_FORMAT_R32_UINT};
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    m_meshes.emplace(kind, std::move(mesh));
    return true;
  };

  auto cubeV = CubeVertices(); auto cubeI = CubeIndices();
  if (!createMesh(LabMeshKind::Cube, cubeV, cubeI)) return false;
  const std::vector<Vertex> planeV{{-1,0,-1,0,1,0,0,1},{-1,0,1,0,1,0,0,0},
                                   {1,0,1,0,1,0,1,0},{1,0,-1,0,1,0,1,1}};
  const std::vector<uint32_t> planeI{0,1,2,0,2,3};
  if (!createMesh(LabMeshKind::Plane, planeV, planeI) ||
      !createMesh(LabMeshKind::Grid, planeV, planeI)) return false;
  const std::vector<Vertex> quadV{{-1,-1,0,0,0,-1,0,1},{1,-1,0,0,0,-1,1,1},
                                  {1,1,0,0,0,-1,1,0},{-1,1,0,0,0,-1,0,0}};
  if (!createMesh(LabMeshKind::Quad, quadV, planeI)) return false;
  std::vector<Vertex> vertices; std::vector<uint32_t> indices;
  MakeSphere(vertices, indices);
  if (!createMesh(LabMeshKind::Sphere, vertices, indices)) return false;
  vertices.clear(); indices.clear(); MakeCylinder(vertices, indices);
  if (!createMesh(LabMeshKind::Cylinder, vertices, indices)) return false;
  vertices.clear(); indices.clear(); MakeDisc(vertices, indices, false);
  if (!createMesh(LabMeshKind::Disc, vertices, indices)) return false;
  vertices.clear(); indices.clear(); MakeDisc(vertices, indices, true);
  return createMesh(LabMeshKind::Ring, vertices, indices);
}

bool Dx12Renderer::CreateUploadArena(std::wstring &error) {
  auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
  auto desc = BufferDesc(kUploadArenaSize * kFrameCount);
  if (!Check(m_device->CreateCommittedResource(
                 &heap, D3D12_HEAP_FLAG_NONE, &desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&m_uploadArena)),
             L"Create constant upload arena", error)) return false;
  return Check(m_uploadArena->Map(0, nullptr,
                                  reinterpret_cast<void **>(&m_uploadMapped)),
               L"Map constant upload arena", error);
}

bool Dx12Renderer::CreateSizeDependentResources(std::wstring &error) {
  for (uint32_t i = 0; i < kFrameCount; ++i) {
    if (!Check(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
               L"Get swap-chain buffer", error)) return false;
    m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, Rtv(i));
  }
  auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  auto makeColor = [&](Texture &texture, DXGI_FORMAT format, uint32_t rtvIndex,
                       uint32_t srvIndex, const float clear[4]) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width; desc.Height = m_height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue{}; clearValue.Format = format;
    std::copy(clear, clear + 4, clearValue.Color);
    if (!Check(m_device->CreateCommittedResource(
                   &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                   IID_PPV_ARGS(&texture.resource)),
               L"Create render texture", error)) return false;
    texture.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    texture.format = format;
    m_device->CreateRenderTargetView(texture.resource.Get(), nullptr,
                                     Rtv(rtvIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    auto cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(srvIndex) * m_srvStride;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
    return true;
  };
  const float black[4]{0,0,0,0}; const float white[4]{1,1,1,1};
  if (!makeColor(m_gbufferAlbedo,kHdrFormat,2,0,black) ||
      !makeColor(m_gbufferNormal,kHdrFormat,3,1,black) ||
      !makeColor(m_gbufferPosition,kHdrFormat,4,2,black) ||
      !makeColor(m_ao,DXGI_FORMAT_R8_UNORM,5,4,white) ||
      !makeColor(m_scene,kHdrFormat,6,5,black) ||
      !makeColor(m_composite,kHdrFormat,7,6,black) ||
      !makeColor(m_bloomA,kHdrFormat,8,7,black) ||
      !makeColor(m_bloomB,kHdrFormat,9,8,black)) return false;

  D3D12_RESOURCE_DESC depthDesc{};
  depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDesc.Width = m_width; depthDesc.Height = m_height;
  depthDesc.DepthOrArraySize = 1; depthDesc.MipLevels = 1;
  depthDesc.Format = DXGI_FORMAT_D32_FLOAT; depthDesc.SampleDesc.Count = 1;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depthClear{}; depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;
  if (!Check(m_device->CreateCommittedResource(
                 &defaultHeap,D3D12_HEAP_FLAG_NONE,&depthDesc,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE,&depthClear,
                 IID_PPV_ARGS(&m_depth)), L"Create main depth",error)) return false;
  m_device->CreateDepthStencilView(m_depth.Get(),nullptr,Dsv(0));

  D3D12_RESOURCE_DESC shadowDesc = depthDesc;
  shadowDesc.Width = kShadowSize; shadowDesc.Height = kShadowSize;
  shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  if (!Check(m_device->CreateCommittedResource(
                 &defaultHeap,D3D12_HEAP_FLAG_NONE,&shadowDesc,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&depthClear,
                 IID_PPV_ARGS(&m_shadow.resource)), L"Create shadow map",error)) return false;
  m_shadow.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  m_shadow.format = DXGI_FORMAT_R32_FLOAT;
  D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsv{};
  shadowDsv.Format = DXGI_FORMAT_D32_FLOAT;
  shadowDsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_device->CreateDepthStencilView(m_shadow.resource.Get(),&shadowDsv,Dsv(1));
  D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrv{};
  shadowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  shadowSrv.Format = DXGI_FORMAT_R32_FLOAT;
  shadowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  shadowSrv.Texture2D.MipLevels = 1;
  auto srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
  srvCpu.ptr += 3u * m_srvStride;
  m_device->CreateShaderResourceView(m_shadow.resource.Get(),&shadowSrv,srvCpu);
  return true;
}

void Dx12Renderer::ReleaseSizeDependentResources() {
  m_depth.Reset(); m_gbufferAlbedo.resource.Reset(); m_gbufferNormal.resource.Reset();
  m_gbufferPosition.resource.Reset(); m_shadow.resource.Reset(); m_ao.resource.Reset();
  m_scene.resource.Reset(); m_composite.resource.Reset(); m_bloomA.resource.Reset();
  m_bloomB.resource.Reset();
}

bool Dx12Renderer::Resize(uint32_t width, uint32_t height, std::wstring &error) {
  width = std::max(1u,width); height = std::max(1u,height);
  if (!m_swapChain || (width == m_width && height == m_height)) return true;
  WaitForGpu();
  ReleaseSizeDependentResources();
  for (auto &buffer : m_backBuffers) buffer.Reset();
  if (!Check(m_swapChain->ResizeBuffers(kFrameCount,width,height,kBackBufferFormat,0),
             L"ResizeBuffers",error)) return false;
  m_width=width; m_height=height;
  m_frameIndex=m_swapChain->GetCurrentBackBufferIndex();
  return CreateSizeDependentResources(error);
}

void Dx12Renderer::WaitForGpu() {
  if (!m_queue || !m_fence || !m_fenceEvent) return;
  const uint64_t value = m_nextFenceValue++;
  if (FAILED(m_queue->Signal(m_fence.Get(), value))) return;
  if (m_fence->GetCompletedValue() < value) {
    m_fence->SetEventOnCompletion(value,m_fenceEvent);
    WaitForSingleObject(m_fenceEvent,INFINITE);
  }
  m_frameFenceValues.fill(0);
}

void Dx12Renderer::WaitForCurrentFrame() {
  const uint64_t value=m_frameFenceValues[m_frameIndex];
  if (value && m_fence->GetCompletedValue()<value) {
    m_fence->SetEventOnCompletion(value,m_fenceEvent);
    WaitForSingleObject(m_fenceEvent,INFINITE);
  }
}

bool Dx12Renderer::CheckDebugMessages(std::wstring &error) {
#ifdef _DEBUG
  if (!m_infoQueue)
    return true;
  const uint64_t messageCount =
      m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
  std::wstring details;
  for (uint64_t index = m_debugMessageCursor; index < messageCount; ++index) {
    SIZE_T bytes = 0;
    m_infoQueue->GetMessage(index, nullptr, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
    if (FAILED(m_infoQueue->GetMessage(index, message, &bytes)))
      continue;
    if (message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
        message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION)
      continue;
    const std::string text(message->pDescription,
                           message->pDescription + message->DescriptionByteLength);
    details += LabUtf8ToWide(text) + L"\n";
  }
  m_debugMessageCursor = messageCount;
  if (!details.empty()) {
    error = L"D3D12 debug layer reported an invalid rendering operation:\n" +
            details;
    return false;
  }
#endif
  return true;
}

Dx12Renderer::UploadAllocation Dx12Renderer::AllocateConstants(const void *data,
                                                               size_t bytes) {
  const size_t aligned=Align256(bytes);
  const size_t frameEnd =
      (static_cast<size_t>(m_frameIndex) + 1u) * kUploadArenaSize;
  if (m_uploadOffset+aligned>frameEnd)
    throw std::runtime_error("DX12 constant upload arena exhausted");
  UploadAllocation result{m_uploadArena->GetGPUVirtualAddress()+m_uploadOffset,
                          m_uploadMapped+m_uploadOffset};
  std::memcpy(result.cpu,data,bytes);
  m_uploadOffset+=aligned;
  return result;
}

void Dx12Renderer::Transition(Texture &texture,D3D12_RESOURCE_STATES next) {
  if (!texture.resource || texture.state==next) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource=texture.resource.Get();
  barrier.Transition.StateBefore=texture.state;
  barrier.Transition.StateAfter=next;
  barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_list->ResourceBarrier(1,&barrier);
  texture.state=next;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::Rtv(uint32_t index) const {
  auto handle=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr+=static_cast<SIZE_T>(index)*m_rtvStride; return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::Dsv(uint32_t index) const {
  auto handle=m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr+=static_cast<SIZE_T>(index)*m_dsvStride; return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE Dx12Renderer::SrvTable() const {
  return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

uint32_t Dx12Renderer::BuildFeatureFlags(const FeatureRegistry &f) const {
  uint32_t flags=0;
  const std::array<std::pair<std::string_view,uint32_t>,17> map{{
    {"directional_light",1u<<0},{"point_lights",1u<<1},{"shadows",1u<<2},
    {"ssao",1u<<3},{"fog",1u<<4},{"normal_mapping",1u<<5},{"pbr",1u<<6},
    {"emissive",1u<<7},{"procedural_material",1u<<8},{"water_waves",1u<<9},
    {"ssr",1u<<10},{"hdr",1u<<11},{"bloom",1u<<12},{"tonemap",1u<<13},
    {"fxaa",1u<<14},{"phase2",1u<<15},{"uv_animation",1u<<16}}};
  for (const auto &[id,bit]:map) if (f.Enabled(id)) flags|=bit;
  if (!f.Enabled("deferred"))
    flags &= ~(1u << 10); // SSR requires the current frame's position G-buffer.
  return flags;
}

Dx12Renderer::FrameConstants Dx12Renderer::BuildFrameConstants(
    const FeatureWorld &world,const FeatureRegistry &features,float dt) {
  if (features.Enabled("camera_orbit") &&
      features.Enabled("camera_auto_move") && features.Enabled("auto_demo"))
    m_yaw+=dt*0.075f;
  m_elapsedTime+=dt;
  const XMVECTOR target=XMVectorSet(0,2.5f,3,1);
  const float horizontal=m_distance*std::cos(m_pitch);
  const XMVECTOR eye=XMVectorSet(std::sin(m_yaw)*horizontal,
      2.5f+std::sin(m_pitch)*m_distance,
      3.0f-std::cos(m_yaw)*horizontal,1);
  const XMMATRIX view=XMMatrixLookAtLH(eye,target,XMVectorSet(0,1,0,0));
  const XMMATRIX projection=XMMatrixPerspectiveFovLH(XMConvertToRadians(58.0f),
      static_cast<float>(m_width)/m_height,0.1f,140.0f);
  const auto &wf=world.Frame();
  XMVECTOR sun=XMVector3Normalize(XMLoadFloat3(&wf.sunDirection));
  XMVECTOR lightEye=target-sun*42.0f;
  const XMMATRIX lightView=XMMatrixLookAtLH(lightEye,target,XMVectorSet(0,1,0,0));
  const XMMATRIX lightProjection=XMMatrixOrthographicLH(64,64,1,110);
  FrameConstants constants{};
  XMStoreFloat4x4(&constants.viewProj,view*projection);
  XMStoreFloat4x4(&constants.lightViewProj,lightView*lightProjection);
  XMFLOAT3 eyePosition{}; XMStoreFloat3(&eyePosition,eye);
  constants.cameraPositionTime={eyePosition.x,eyePosition.y,eyePosition.z,m_elapsedTime};
  XMVECTOR inverseDet{};
  XMMATRIX invView=XMMatrixInverse(&inverseDet,view);
  XMFLOAT3 right{},up{};
  XMStoreFloat3(&right,invView.r[0]); XMStoreFloat3(&up,invView.r[1]);
  constants.cameraRightViewportX={right.x,right.y,right.z,1.0f/m_width};
  constants.cameraUpViewportY={up.x,up.y,up.z,1.0f/m_height};
  constants.sunDirectionIntensity={wf.sunDirection.x,wf.sunDirection.y,
                                   wf.sunDirection.z,wf.sunIntensity};
  constants.sunColor={wf.sunColor.x,wf.sunColor.y,wf.sunColor.z,1};
  constants.pointLightCount=static_cast<uint32_t>(std::min<size_t>(8,wf.pointLights.size()));
  for(uint32_t i=0;i<constants.pointLightCount;++i){
    const auto &light=wf.pointLights[i];
    constants.pointLights[i].positionRange={light.position.x,light.position.y,light.position.z,light.range};
    constants.pointLights[i].colorIntensity={light.color.x,light.color.y,light.color.z,light.intensity};
  }
  constants.featureFlags=BuildFeatureFlags(features);
  constants.attackType=static_cast<uint32_t>(wf.attack);
  constants.phaseTwo=wf.phaseTwo?1u:0u;
  return constants;
}

bool Dx12Renderer::IsEntityVisible(const LabEntity &entity,
                                   const FeatureRegistry &features) const {
  if (!features.Enabled("entity_system") || !entity.active ||
      !features.Enabled(entity.featureId)) return false;
  if ((entity.flags&LabEntityTelegraph)!=0 && !features.Enabled("boss")) return false;
  if (entity.name=="CollisionMover" && !features.Enabled("collision")) return false;
  return true;
}

uint32_t Dx12Renderer::ObjectType(const LabEntity &entity) const {
  if (entity.name=="InstancedForest") return 1;
  if (entity.name=="SparkParticles") return 2;
  if (entity.name=="RainParticles") return 3;
  if (entity.mesh==LabMeshKind::Grid) return 4;
  if (entity.name=="CounterBurst") return 5;
  return 0;
}

void Dx12Renderer::DrawEntities(const FeatureWorld &world,
                                const FeatureRegistry &features,
                                D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                                bool transparent,bool shadowPass) {
  m_list->SetGraphicsRootConstantBufferView(0,frameCb);
  m_list->SetGraphicsRootDescriptorTable(2,SrvTable());
  for(const auto &entity:world.Entities()){
    if(!IsEntityVisible(entity,features)) continue;
    const bool isTransparent=(entity.flags&LabEntityTransparent)!=0;
    if(isTransparent!=transparent) continue;
    if(shadowPass && ((entity.flags&LabEntityNoShadow)!=0 || isTransparent)) continue;
    auto found=m_meshes.find(entity.mesh); if(found==m_meshes.end()) continue;
    ObjectConstants object{};
    XMStoreFloat4x4(&object.world,entity.transform.Matrix());
    object.baseColor=entity.material.baseColor;
    object.materialParams={entity.material.metallic,entity.material.roughness,
                           entity.material.emissive,entity.material.pattern};
    object.entityFlags=entity.flags;
    object.objectType=ObjectType(entity);
    object.instanceCount=std::max(1u,entity.instanceCount);
    const auto allocation=AllocateConstants(&object,sizeof(object));
    m_list->SetGraphicsRootConstantBufferView(1,allocation.gpu);
    const auto &mesh=found->second;
    m_list->IASetVertexBuffers(0,1,&mesh.vertexView);
    m_list->IASetIndexBuffer(&mesh.indexView);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawIndexedInstanced(mesh.indexCount,object.instanceCount,0,0,0);
    ++m_drawCalls;
  }
}

void Dx12Renderer::DrawFullscreen(ID3D12PipelineState *pipeline,
                                  D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                                  D3D12_CPU_DESCRIPTOR_HANDLE target,
                                  DXGI_FORMAT) {
  m_list->SetPipelineState(pipeline);
  m_list->SetGraphicsRootSignature(m_rootSignature.Get());
  m_list->SetGraphicsRootConstantBufferView(0,frameCb);
  m_list->SetGraphicsRootDescriptorTable(2,SrvTable());
  m_list->OMSetRenderTargets(1,&target,FALSE,nullptr);
  m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_list->DrawInstanced(3,1,0,0); ++m_drawCalls;
}

bool Dx12Renderer::Render(const FeatureWorld &world,
                          const FeatureRegistry &features,float dt,
                          std::wstring &error) {
  if(!m_swapChain || m_width==0 || m_height==0) return true;
  const auto start=std::chrono::steady_clock::now();
  WaitForCurrentFrame();
  if(!Check(m_allocators[m_frameIndex]->Reset(),L"Reset command allocator",error) ||
     !Check(m_list->Reset(m_allocators[m_frameIndex].Get(),nullptr),L"Reset command list",error))
    return false;
  m_uploadOffset=static_cast<size_t>(m_frameIndex)*kUploadArenaSize;
  m_drawCalls=0;
  try {
    const FrameConstants frame=BuildFrameConstants(world,features,dt);
    const auto frameAllocation=AllocateConstants(&frame,sizeof(frame));
    ID3D12DescriptorHeap *heaps[]{m_srvHeap.Get()};
    m_list->SetDescriptorHeaps(1,heaps);
    m_list->SetGraphicsRootSignature(m_rootSignature.Get());
    const D3D12_VIEWPORT viewport{0,0,static_cast<float>(m_width),static_cast<float>(m_height),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(m_width),static_cast<LONG>(m_height)};

    if(features.Enabled("shadows")){
      Transition(m_shadow,D3D12_RESOURCE_STATE_DEPTH_WRITE);
      const D3D12_VIEWPORT shadowViewport{0,0,static_cast<float>(kShadowSize),static_cast<float>(kShadowSize),0,1};
      const D3D12_RECT shadowScissor{0,0,static_cast<LONG>(kShadowSize),static_cast<LONG>(kShadowSize)};
      m_list->RSSetViewports(1,&shadowViewport); m_list->RSSetScissorRects(1,&shadowScissor);
      auto shadowDsv=Dsv(1); m_list->OMSetRenderTargets(0,nullptr,FALSE,&shadowDsv);
      m_list->ClearDepthStencilView(shadowDsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
      m_list->SetPipelineState(m_shadowPso.Get());
      DrawEntities(world,features,frameAllocation.gpu,false,true);
      Transition(m_shadow,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_list->RSSetViewports(1,&viewport); m_list->RSSetScissorRects(1,&scissor);
    const bool deferred=features.Enabled("deferred");
    const bool wire=features.Enabled("wireframe");
    auto depthDsv=Dsv(0);
    if(deferred){
      Transition(m_gbufferAlbedo,D3D12_RESOURCE_STATE_RENDER_TARGET);
      Transition(m_gbufferNormal,D3D12_RESOURCE_STATE_RENDER_TARGET);
      Transition(m_gbufferPosition,D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE targets[]{Rtv(2),Rtv(3),Rtv(4)};
      const float clear[4]{0,0,0,0};
      for(auto target:targets) m_list->ClearRenderTargetView(target,clear,0,nullptr);
      m_list->ClearDepthStencilView(depthDsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
      m_list->OMSetRenderTargets(3,targets,FALSE,&depthDsv);
      m_list->SetPipelineState(wire?m_gbufferWirePso.Get():m_gbufferPso.Get());
      DrawEntities(world,features,frameAllocation.gpu,false,false);
      Transition(m_gbufferAlbedo,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_gbufferNormal,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_gbufferPosition,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_ao,D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float white[4]{1,1,1,1}; m_list->ClearRenderTargetView(Rtv(5),white,0,nullptr);
      DrawFullscreen(m_ssaoPso.Get(),frameAllocation.gpu,Rtv(5),DXGI_FORMAT_R8_UNORM);
      Transition(m_ao,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_scene,D3D12_RESOURCE_STATE_RENDER_TARGET);
      DrawFullscreen(m_deferredPso.Get(),frameAllocation.gpu,Rtv(6),kHdrFormat);
      Transition(m_scene,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else {
      Transition(m_scene,D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float sky[4]{0.008f,0.025f,0.04f,1};
      m_list->ClearRenderTargetView(Rtv(6),sky,0,nullptr);
      m_list->ClearDepthStencilView(depthDsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
      auto sceneRtv = Rtv(6);
      m_list->OMSetRenderTargets(1, &sceneRtv, FALSE, &depthDsv);
      m_list->SetPipelineState(wire?m_forwardWirePso.Get():m_forwardPso.Get());
      DrawEntities(world,features,frameAllocation.gpu,false,false);
      Transition(m_scene,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    Transition(m_composite,D3D12_RESOURCE_STATE_RENDER_TARGET);
    DrawFullscreen(m_copyPso.Get(),frameAllocation.gpu,Rtv(7),kHdrFormat);
    auto compositeRtv = Rtv(7);
    m_list->OMSetRenderTargets(1, &compositeRtv, FALSE, &depthDsv);
    m_list->SetPipelineState(m_transparentPso.Get());
    DrawEntities(world,features,frameAllocation.gpu,true,false);
    Transition(m_composite,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if(features.Enabled("bloom")){
      Transition(m_bloomA,D3D12_RESOURCE_STATE_RENDER_TARGET);
      DrawFullscreen(m_bloomExtractPso.Get(),frameAllocation.gpu,Rtv(8),kHdrFormat);
      Transition(m_bloomA,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_bloomB,D3D12_RESOURCE_STATE_RENDER_TARGET);
      DrawFullscreen(m_bloomBlurHPso.Get(),frameAllocation.gpu,Rtv(9),kHdrFormat);
      Transition(m_bloomB,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(m_bloomA,D3D12_RESOURCE_STATE_RENDER_TARGET);
      DrawFullscreen(m_bloomBlurVPso.Get(),frameAllocation.gpu,Rtv(8),kHdrFormat);
      Transition(m_bloomA,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    D3D12_RESOURCE_BARRIER backToRtv{};
    backToRtv.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backToRtv.Transition.pResource=m_backBuffers[m_frameIndex].Get();
    backToRtv.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;
    backToRtv.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    backToRtv.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_list->ResourceBarrier(1,&backToRtv);
    DrawFullscreen(m_tonemapPso.Get(),frameAllocation.gpu,Rtv(m_frameIndex),kBackBufferFormat);
    std::swap(backToRtv.Transition.StateBefore,backToRtv.Transition.StateAfter);
    m_list->ResourceBarrier(1,&backToRtv);
  } catch(const std::exception &exception) {
    error=std::wstring(exception.what(),exception.what()+std::strlen(exception.what()));
    m_list->Close(); return false;
  }

  if(!Check(m_list->Close(),L"Close command list",error)) return false;
  ID3D12CommandList *lists[]{m_list.Get()}; m_queue->ExecuteCommandLists(1,lists);
  if(!Check(m_swapChain->Present(1,0),L"Present",error)) return false;
  const uint64_t fenceValue=m_nextFenceValue++;
  if(!Check(m_queue->Signal(m_fence.Get(),fenceValue),L"Signal frame fence",error)) return false;
  m_frameFenceValues[m_frameIndex]=fenceValue;
  m_frameIndex=m_swapChain->GetCurrentBackBufferIndex();
  if (!CheckDebugMessages(error))
    return false;
  m_lastFrameMilliseconds=std::chrono::duration<float,std::milli>(
      std::chrono::steady_clock::now()-start).count();
  return true;
}

void Dx12Renderer::Orbit(float yaw,float pitch){
  m_yaw+=yaw; m_pitch=std::clamp(m_pitch+pitch,-1.15f,1.15f);
}
void Dx12Renderer::Zoom(float delta){m_distance=std::clamp(m_distance-delta,9.0f,65.0f);}
void Dx12Renderer::ResetCamera(){m_yaw=0;m_pitch=0.24f;m_distance=31.0f;}
