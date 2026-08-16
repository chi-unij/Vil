// ======================================
// File: HybridReflectionRenderer.cpp
// Purpose: Validate the DXR 1.1 and DXIL prerequisites without changing
//          the existing raster/SSR frame output.
// ======================================

#include "HybridReflectionRenderer.h"

#include "DxContext.h"
#include "MeshRenderer.h"
#include "RenderPass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <windows.h>

#ifndef VILLIEN_DXR_SHADER_BUILD
#define VILLIEN_DXR_SHADER_BUILD 0
#endif

namespace {
bool CreateBuffer(ID3D12Device *device, uint64_t byteSize,
                  D3D12_HEAP_TYPE heapType,
                  D3D12_RESOURCE_FLAGS resourceFlags,
                  D3D12_RESOURCE_STATES initialState,
                  Microsoft::WRL::ComPtr<ID3D12Resource> &resource) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heapType;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = std::max<uint64_t>(byteSize, 256u);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = resourceFlags;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
      IID_PPV_ARGS(&resource)));
}

std::filesystem::path CompiledShaderPath() {
  std::array<wchar_t, 32768> executablePath{};
  const DWORD length = GetModuleFileNameW(
      nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
  if (length == 0 || length >= executablePath.size())
    return {};

  return std::filesystem::path(executablePath.data()).parent_path() /
         L"shaders" / L"hybrid_reflection.dxil";
}
} // namespace

bool HybridReflectionRenderer::Initialize(DxContext &dx) {
  Reset();

#if VILLIEN_DXR_SHADER_BUILD
  std::error_code pathError;
  const std::filesystem::path shaderPath = CompiledShaderPath();
  m_shaderAvailable = !shaderPath.empty() &&
                      std::filesystem::is_regular_file(shaderPath, pathError);
#else
  m_shaderAvailable = false;
#endif

  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  const HRESULT featureResult = dx.Device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
  if (FAILED(featureResult)) {
    m_status = "DXR capability query に失敗しました。SSR を使用します。";
    return false;
  }

  m_raytracingTier = options5.RaytracingTier;
  if (m_raytracingTier < D3D12_RAYTRACING_TIER_1_1) {
    m_status = "DXR Tier 1.1 は未対応です。SSR を使用します。";
    return false;
  }

  if (FAILED(dx.Device()->QueryInterface(IID_PPV_ARGS(&m_device5))) ||
      FAILED(dx.CmdList()->QueryInterface(IID_PPV_ARGS(&m_commandList4)))) {
    m_device5.Reset();
    m_commandList4.Reset();
    m_status = "DXR interface の取得に失敗しました。SSR を使用します。";
    return false;
  }

  if (!m_shaderAvailable) {
    m_status = "DXR hardware は利用可能ですが、DXIL shader がありません。SSR を使用します。";
    return false;
  }

  if (!CreateProofPipeline(dx))
    return false;

  m_supported = true;
  m_status = "DXR Tier 1.1 と DXIL shader を確認しました。";
  return true;
}

bool HybridReflectionRenderer::CreateProofPipeline(DxContext &dx) {
  const std::filesystem::path shaderPath = CompiledShaderPath();
  std::ifstream shaderFile(shaderPath, std::ios::binary | std::ios::ate);
  if (!shaderFile) {
    m_status = "DXIL shader を開けませんでした。SSR を使用します。";
    return false;
  }

  const std::streamsize shaderSize = shaderFile.tellg();
  if (shaderSize <= 0) {
    m_status = "DXIL shader が空です。SSR を使用します。";
    return false;
  }

  std::vector<uint8_t> shaderBytecode(static_cast<size_t>(shaderSize));
  shaderFile.seekg(0, std::ios::beg);
  if (!shaderFile.read(reinterpret_cast<char *>(shaderBytecode.data()),
                       shaderSize)) {
    m_status = "DXIL shader の読み込みに失敗しました。SSR を使用します。";
    return false;
  }

  D3D12_DESCRIPTOR_RANGE outputRange{};
  outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  outputRange.NumDescriptors = 1;
  outputRange.BaseShaderRegister = 0;
  outputRange.RegisterSpace = 0;
  outputRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE inputRanges[4]{};
  for (uint32_t index = 0; index < std::size(inputRanges); ++index) {
    inputRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    inputRanges[index].NumDescriptors = 1;
    inputRanges[index].BaseShaderRegister = index + 1;
    inputRanges[index].RegisterSpace = 0;
    inputRanges[index].OffsetInDescriptorsFromTableStart = 0;
  }

  D3D12_ROOT_PARAMETER parameters[8]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].Descriptor.RegisterSpace = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &outputRange;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[2].Descriptor.ShaderRegister = 0;
  parameters[2].Descriptor.RegisterSpace = 0;
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  for (uint32_t index = 0; index < std::size(inputRanges); ++index) {
    parameters[3 + index].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3 + index].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3 + index].DescriptorTable.pDescriptorRanges =
        &inputRanges[index];
    parameters[3 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // t5: InstanceID から参照する per-instance base color buffer。
  parameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[7].Descriptor.ShaderRegister = 5;
  parameters[7].Descriptor.RegisterSpace = 0;
  parameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
  rootDesc.pParameters = parameters;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> rootBlob;
  Microsoft::WRL::ComPtr<ID3DBlob> rootError;
  if (FAILED(D3D12SerializeRootSignature(
          &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootError)) ||
      FAILED(dx.Device()->CreateRootSignature(
          0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
          IID_PPV_ARGS(&m_rootSignature)))) {
    m_rootSignature.Reset();
    m_status = "DXR proof root signature の作成に失敗しました。SSR を使用します。";
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature = m_rootSignature.Get();
  pipelineDesc.CS = {shaderBytecode.data(), shaderBytecode.size()};
  if (FAILED(dx.Device()->CreateComputePipelineState(
          &pipelineDesc, IID_PPV_ARGS(&m_computePso)))) {
    m_computePso.Reset();
    m_rootSignature.Reset();
    m_status = "DXR proof compute pipeline の作成に失敗しました。SSR を使用します。";
    return false;
  }

  return true;
}

bool HybridReflectionRenderer::EnsureOutput(DxContext &dx) {
  const uint32_t width = dx.Width();
  const uint32_t height = dx.Height();
  if (width == 0 || height == 0)
    return false;
  if (m_outputTexture && m_outputWidth == width && m_outputHeight == height)
    return true;

  m_outputTexture.Reset();

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = width;
  textureDesc.Height = height;
  textureDesc.DepthOrArraySize = 1;
  textureDesc.MipLevels = 1;
  textureDesc.Format = dx.HdrFormat();
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (FAILED(dx.Device()->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &textureDesc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&m_outputTexture)))) {
    m_status = "DXR proof output texture の作成に失敗しました。SSR を使用します。";
    return false;
  }
  m_outputTexture->SetName(L"HybridReflection.ProofOutput");

  if (!m_outputDescriptorsAllocated) {
    m_outputUavCpu = dx.AllocMainSrvCpu(2);
    m_outputSrvCpu = m_outputUavCpu;
    m_outputSrvCpu.ptr += dx.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_outputUavGpu = dx.MainSrvGpuFromCpu(m_outputUavCpu);
    m_outputSrvGpu = dx.MainSrvGpuFromCpu(m_outputSrvCpu);
    m_outputDescriptorsAllocated = true;
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
  uavDesc.Format = dx.HdrFormat();
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  dx.Device()->CreateUnorderedAccessView(m_outputTexture.Get(), nullptr,
                                         &uavDesc, m_outputUavCpu);

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Format = dx.HdrFormat();
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  dx.Device()->CreateShaderResourceView(m_outputTexture.Get(), &srvDesc,
                                        m_outputSrvCpu);

  m_outputWidth = width;
  m_outputHeight = height;
  return true;
}

bool HybridReflectionRenderer::BuildBlas(
    DxContext &dx, uint32_t meshId, const MeshRenderer &meshRenderer) {
  if (m_blasCache.contains(meshId))
    return true;

  MeshRenderer::RayTracingGeometryView geometryView{};
  if (!meshRenderer.GetRayTracingGeometry(meshId, geometryView))
    return false;

  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  geometry.Triangles.VertexBuffer.StartAddress = geometryView.vertexBuffer;
  geometry.Triangles.VertexBuffer.StrideInBytes = geometryView.vertexStride;
  geometry.Triangles.VertexCount = geometryView.vertexCount;
  geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geometry.Triangles.IndexBuffer = geometryView.indexBuffer;
  geometry.Triangles.IndexCount = geometryView.indexCount;
  geometry.Triangles.IndexFormat = geometryView.indexFormat;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  inputs.NumDescs = 1;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = &geometry;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

  BlasResources resources{};
  if (info.ResultDataMaxSizeInBytes == 0 ||
      !CreateBuffer(m_device5.Get(), info.ScratchDataSizeInBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON, resources.scratch) ||
      !CreateBuffer(
          m_device5.Get(), info.ResultDataMaxSizeInBytes,
          D3D12_HEAP_TYPE_DEFAULT,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
          resources.result)) {
    m_status = "BLAS resource の作成に失敗しました。SSR を使用します。";
    return false;
  }

  const std::wstring nameSuffix = std::to_wstring(meshId);
  resources.scratch->SetName(
      (L"HybridReflection.BLAS.Scratch." + nameSuffix).c_str());
  resources.result->SetName(
      (L"HybridReflection.BLAS.Result." + nameSuffix).c_str());
  dx.Transition(resources.scratch.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = inputs;
  build.ScratchAccelerationStructureData =
      resources.scratch->GetGPUVirtualAddress();
  build.DestAccelerationStructureData =
      resources.result->GetGPUVirtualAddress();
  m_commandList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resources.result.Get();
  m_commandList4->ResourceBarrier(1, &barrier);

  m_blasCache.emplace(meshId, std::move(resources));
  return true;
}

bool HybridReflectionRenderer::PrepareScene(
    DxContext &dx, const FrameData &frame,
    const MeshRenderer &meshRenderer) {
  if (!m_supported)
    return false;

  std::vector<SceneInstance> collectedInstances;
  collectedInstances.reserve(frame.opaqueItems.size());
  for (const RenderItem &item : frame.opaqueItems) {
    MeshRenderer::RayTracingGeometryView geometryView{};
    if (!meshRenderer.GetRayTracingGeometry(item.meshId, geometryView) ||
        geometryView.hasSkeleton || geometryView.hasVertexDeformation ||
        !geometryView.rayTracingVisible) {
      continue;
    }

    SceneInstance instance{};
    instance.meshId = item.meshId;
    DirectX::XMStoreFloat4x4(&instance.world, item.world);
    instance.baseColor = geometryView.baseColor;
    collectedInstances.push_back(instance);
  }

  bool sceneUnchanged = m_sceneReady &&
                        collectedInstances.size() == m_sceneInstances.size();
  if (sceneUnchanged) {
    for (size_t index = 0; index < collectedInstances.size(); ++index) {
      const SceneInstance &current = collectedInstances[index];
      const SceneInstance &previous = m_sceneInstances[index];
      if (current.meshId != previous.meshId ||
          std::memcmp(&current.world, &previous.world,
                      sizeof(current.world)) != 0 ||
          std::memcmp(&current.baseColor, &previous.baseColor,
                      sizeof(current.baseColor)) != 0) {
        sceneUnchanged = false;
        break;
      }
    }
  }
  if (sceneUnchanged)
    return true;

  if (m_tlasResult || m_instanceUpload || m_instanceColorUpload) {
    dx.WaitForGpu();
    ResetScene();
  }
  if (collectedInstances.empty()) {
    m_status = "DXR 対象の static opaque mesh がありません。SSR を使用します。";
    return false;
  }

  for (const SceneInstance &instance : collectedInstances) {
    if (!BuildBlas(dx, instance.meshId, meshRenderer))
      return false;
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  tlasInputs.NumDescs = static_cast<UINT>(collectedInstances.size());
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
  m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs,
                                                             &tlasInfo);
  const uint64_t instanceBytes =
      sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * collectedInstances.size();
  const uint64_t colorBytes =
      sizeof(DirectX::XMFLOAT4) * collectedInstances.size();
  if (tlasInfo.ResultDataMaxSizeInBytes == 0 ||
      !CreateBuffer(m_device5.Get(), tlasInfo.ScratchDataSizeInBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON, m_tlasScratch) ||
      !CreateBuffer(
          m_device5.Get(), tlasInfo.ResultDataMaxSizeInBytes,
          D3D12_HEAP_TYPE_DEFAULT,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
          m_tlasResult) ||
      !CreateBuffer(m_device5.Get(), instanceBytes, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ, m_instanceUpload) ||
      !CreateBuffer(m_device5.Get(), colorBytes, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    m_instanceColorUpload)) {
    m_status = "TLAS resource の作成に失敗しました。SSR を使用します。";
    ResetScene();
    return false;
  }

  m_tlasScratch->SetName(L"HybridReflection.TLAS.Scratch");
  m_tlasResult->SetName(L"HybridReflection.TLAS.Result");
  m_instanceUpload->SetName(L"HybridReflection.TLAS.Instances");
  m_instanceColorUpload->SetName(L"HybridReflection.InstanceColors");
  dx.Transition(m_tlasScratch.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  void *mappedInstances = nullptr;
  void *mappedColors = nullptr;
  const HRESULT instanceMapResult =
      m_instanceUpload->Map(0, nullptr, &mappedInstances);
  const HRESULT colorMapResult =
      m_instanceColorUpload->Map(0, nullptr, &mappedColors);
  if (FAILED(instanceMapResult) || FAILED(colorMapResult)) {
    if (SUCCEEDED(instanceMapResult))
      m_instanceUpload->Unmap(0, nullptr);
    if (SUCCEEDED(colorMapResult))
      m_instanceColorUpload->Unmap(0, nullptr);
    m_status = "TLAS instance metadata upload に失敗しました。SSR を使用します。";
    ResetScene();
    return false;
  }

  auto *instanceDescs =
      static_cast<D3D12_RAYTRACING_INSTANCE_DESC *>(mappedInstances);
  auto *instanceColors = static_cast<DirectX::XMFLOAT4 *>(mappedColors);
  for (uint32_t index = 0;
       index < static_cast<uint32_t>(collectedInstances.size()); ++index) {
    const SceneInstance &sceneInstance = collectedInstances[index];
    D3D12_RAYTRACING_INSTANCE_DESC &instanceDesc = instanceDescs[index];
    instanceDesc = {};

    DirectX::XMFLOAT4X4 transform{};
    const DirectX::XMMATRIX world =
        DirectX::XMLoadFloat4x4(&sceneInstance.world);
    DirectX::XMStoreFloat4x4(&transform, DirectX::XMMatrixTranspose(world));
    for (uint32_t row = 0; row < 3; ++row) {
      for (uint32_t column = 0; column < 4; ++column)
        instanceDesc.Transform[row][column] = transform.m[row][column];
    }
    instanceDesc.InstanceID = index;
    instanceDesc.InstanceMask = 0xff;
    instanceDesc.InstanceContributionToHitGroupIndex = 0;
    instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    instanceDesc.AccelerationStructure =
        m_blasCache.at(sceneInstance.meshId).result->GetGPUVirtualAddress();
    instanceColors[index] = sceneInstance.baseColor;
  }
  m_instanceUpload->Unmap(0, nullptr);
  m_instanceColorUpload->Unmap(0, nullptr);

  tlasInputs.InstanceDescs = m_instanceUpload->GetGPUVirtualAddress();
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild{};
  tlasBuild.Inputs = tlasInputs;
  tlasBuild.ScratchAccelerationStructureData =
      m_tlasScratch->GetGPUVirtualAddress();
  tlasBuild.DestAccelerationStructureData =
      m_tlasResult->GetGPUVirtualAddress();
  m_commandList4->BuildRaytracingAccelerationStructure(&tlasBuild, 0, nullptr);

  D3D12_RESOURCE_BARRIER tlasBarrier{};
  tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  tlasBarrier.UAV.pResource = m_tlasResult.Get();
  m_commandList4->ResourceBarrier(1, &tlasBarrier);

  m_sceneInstances = std::move(collectedInstances);
  m_sceneReady = true;
  m_status = "static opaque scene の BLAS cache / TLAS を構築しました。";
  return true;
}

bool HybridReflectionRenderer::Execute(DxContext &dx,
                                       const FrameData &frame) {
  if (!m_supported || !m_sceneReady || !m_tlasResult ||
      !m_instanceColorUpload || !m_rootSignature || !m_computePso ||
      !EnsureOutput(dx)) {
    return false;
  }

  struct ProofConstants {
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 cameraAndMaxDistance;
    DirectX::XMFLOAT4 reflectionParams;
  };

  ProofConstants constants{};
  const DirectX::XMMATRIX viewProj = frame.view * frame.proj;
  DirectX::XMStoreFloat4x4(
      &constants.invViewProj,
      DirectX::XMMatrixTranspose(
          DirectX::XMMatrixInverse(nullptr, viewProj)));
  DirectX::XMStoreFloat4x4(&constants.viewProj,
                           DirectX::XMMatrixTranspose(viewProj));
  constants.cameraAndMaxDistance = {frame.cameraPos.x, frame.cameraPos.y,
                                    frame.cameraPos.z,
                                    std::max(frame.ssrReflectionParams.y,
                                             1.0f)};
  constants.reflectionParams = {frame.ssrReflectionParams.x,
                                frame.ssrReflectionParams.z,
                                frame.ssrReflectionParams.w, 0.0f};

  void *constantCpu = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS constantGpu =
      dx.AllocFrameConstants(sizeof(constants), &constantCpu);
  std::memcpy(constantCpu, &constants, sizeof(constants));

  // Compute shader から scene color / G-buffer / depth を読み取れる状態にする。
  dx.Transition(dx.HdrTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferNormalTarget(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferMaterialTarget(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.DepthBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12DescriptorHeap *descriptorHeaps[] = {dx.MainSrvHeap()};
  m_commandList4->SetDescriptorHeaps(1, descriptorHeaps);
  m_commandList4->SetComputeRootSignature(m_rootSignature.Get());
  m_commandList4->SetPipelineState(m_computePso.Get());
  m_commandList4->SetComputeRootShaderResourceView(
      0, m_tlasResult->GetGPUVirtualAddress());
  m_commandList4->SetComputeRootDescriptorTable(1, m_outputUavGpu);
  m_commandList4->SetComputeRootConstantBufferView(2, constantGpu);
  m_commandList4->SetComputeRootDescriptorTable(3, dx.HdrSrvGpu());
  m_commandList4->SetComputeRootDescriptorTable(4,
                                                dx.GBufferNormalSrvGpu());
  m_commandList4->SetComputeRootDescriptorTable(5,
                                                dx.GBufferMaterialSrvGpu());
  m_commandList4->SetComputeRootDescriptorTable(6, dx.DepthSrvGpu());
  m_commandList4->SetComputeRootShaderResourceView(
      7, m_instanceColorUpload->GetGPUVirtualAddress());
  m_commandList4->Dispatch((m_outputWidth + 7u) / 8u,
                           (m_outputHeight + 7u) / 8u, 1);

  D3D12_RESOURCE_BARRIER outputBarrier{};
  outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  outputBarrier.UAV.pResource = m_outputTexture.Get();
  m_commandList4->ResourceBarrier(1, &outputBarrier);

  // Hybrid output を HDR scene に戻し、後段 pass は通常の経路を維持する。
  dx.Transition(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
  dx.Transition(dx.HdrTarget(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
  m_commandList4->CopyResource(dx.HdrTarget(), m_outputTexture.Get());
  dx.Transition(m_outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dx.Transition(dx.HdrTarget(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
  dx.Transition(dx.GBufferNormalTarget(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.GBufferMaterialTarget(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  dx.Transition(dx.DepthBuffer(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);

  m_status = "DXR reflection を reflection receiver へ合成しました。";
  return true;
}

void HybridReflectionRenderer::ResetScene() {
  m_instanceColorUpload.Reset();
  m_instanceUpload.Reset();
  m_tlasResult.Reset();
  m_tlasScratch.Reset();
  m_sceneInstances.clear();
  m_sceneReady = false;
}

void HybridReflectionRenderer::Reset() {
  m_outputTexture.Reset();
  m_computePso.Reset();
  m_rootSignature.Reset();
  ResetScene();
  m_blasCache.clear();
  m_commandList4.Reset();
  m_device5.Reset();
  m_supported = false;
  m_shaderAvailable = false;
  m_outputUavCpu = {};
  m_outputSrvCpu = {};
  m_outputUavGpu = {};
  m_outputSrvGpu = {};
  m_outputWidth = 0;
  m_outputHeight = 0;
  m_outputDescriptorsAllocated = false;
  m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
  m_status = "DXR は未初期化です。";
}
