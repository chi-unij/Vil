// ======================================
// File: RenderPass.h
// Purpose: Render pass interface + per-frame data structs (Phase 8: formalized
//          render passes with explicit ordering)
// ======================================

#pragma once

#include "Lighting.h"
#include "ShadowMap.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>

class DxContext;
class Emitter;

// A single drawable mesh reference (mesh ID + world transform).
struct RenderItem {
  uint32_t meshId = UINT32_MAX;
  DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
};

struct DebugLine {
  DirectX::XMFLOAT3 start = {};
  DirectX::XMFLOAT3 end = {};
  DirectX::XMFLOAT4 color = {1.0f, 0.2f, 0.1f, 1.0f};
};

// A batch of instances sharing the same mesh (Phase 12.5 — Instanced Rendering).
// Built from RenderItems by grouping on meshId.
struct InstanceBatch {
  uint32_t meshId = UINT32_MAX;
  std::vector<DirectX::XMMATRIX> worldMatrices;
};

// Bundles all per-frame state that render passes need.
// Built fresh each frame in main.cpp and passed to every pass.
struct FrameData {
  // Camera
  DirectX::XMMATRIX view = DirectX::XMMatrixIdentity();
  DirectX::XMMATRIX proj = DirectX::XMMatrixIdentity();
  DirectX::XMFLOAT3 cameraPos = {};

  // Lighting
  LightParams lighting = {};

  // Shadow config (CSM)
  bool shadowsEnabled = false;
  uint32_t cascadeCount = kDefaultCascades;
  std::array<DirectX::XMMATRIX, kMaxCascades> cascadeLightViewProj = {};
  std::array<float, kMaxCascades> cascadeSplitDistances = {};
  float shadowBias = 0.0015f;
  float shadowStrength = 1.0f;

  // Sky
  float skyExposure = 1.0f;

  // Scene items to draw (both shadow + opaque passes iterate this).
  std::vector<RenderItem> opaqueItems;

  // Transparent meshes rendered after deferred lighting. These read depth but
  // do not write it, so underlying opaque surfaces stay visible.
  std::vector<RenderItem> transparentItems;

  // Wireframe highlight overlay for selected entities (editor).
  std::vector<RenderItem> highlightItems;

  // Runtime debug line overlay.
  std::vector<DebugLine> debugLines;
  bool gridEnabled = true;

  // Particles
  bool particlesEnabled = false;
  std::vector<const Emitter*> emitters;

  // Backbuffer clear color
  float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  // Post-processing (Phase 9)
  float exposure = 1.0f;
  float bloomThreshold = 1.0f;
  float bloomIntensity = 0.5f;
  bool bloomEnabled = true;
  bool fxaaEnabled = true;

  // SSAO (Phase 10.3)
  bool ssaoEnabled = true;
  float ssaoRadius = 0.5f;
  float ssaoBias = 0.025f;
  float ssaoPower = 2.0f;
  int ssaoKernelSize = 32;
  float ssaoStrength = 1.0f;

  // Motion blur (Phase 10.5)
  bool motionBlurEnabled = false;
  float motionBlurStrength = 1.0f;
  int motionBlurSamples = 8;
  DirectX::XMMATRIX invViewProj = DirectX::XMMatrixIdentity();
  DirectX::XMMATRIX prevViewProj = DirectX::XMMatrixIdentity();
  bool hasPrevViewProj = false;

  // Depth of Field (Phase 10.6)
  bool dofEnabled = false;
  float dofFocalDistance = 10.0f;
  float dofFocalRange = 5.0f;
  float dofMaxBlur = 8.0f;

  // Point & spot lights (Phase 12.2)
  std::vector<GPUPointLight> pointLights;
  std::vector<GPUSpotLight>  spotLights;

  // TAA (Phase 10.4)
  bool taaEnabled = false;
  float taaBlendFactor = 0.05f;
  DirectX::XMMATRIX invViewProjUnjittered = DirectX::XMMatrixIdentity();
  DirectX::XMMATRIX prevViewProjUnjittered = DirectX::XMMatrixIdentity();

  // Procedural tile animation (Phase 9)
  float gameTime = 0.0f;
  DirectX::XMFLOAT4 waterWaveParams = {1.0f, 1.0f, 1.0f, 0.0f};
  DirectX::XMFLOAT4 wetSurfaceParams = {0.9f, 4.0f, 4.5f, 5.5f};
  DirectX::XMFLOAT4 puddleParams = {0.85f, 5.0f, 3.2f, 1.0f};
  DirectX::XMFLOAT4 puddleVisualParams = {0.25f, 0.60f, 1.0f, 0.0f};
  bool ssrEnabled = true;
  DirectX::XMFLOAT4 ssrReflectionParams = {0.85f, 28.0f, 0.35f, 0.35f};
};

// Base class for all render passes.
// Each pass receives a DxContext (for GPU commands) and a FrameData (read-only
// scene description). Passes are explicitly ordered in main.cpp.
class RenderPass {
public:
  virtual ~RenderPass() = default;
  virtual void Execute(DxContext &dx, const FrameData &frame) = 0;
};
