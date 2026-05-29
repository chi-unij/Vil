// ======================================
// File: RenderPasses.h
// Purpose: Concrete render pass classes (Phase 8). Each pass is a thin
//          orchestrator that calls existing renderer modules in the right
//          order with the right state.
// ======================================

#pragma once

#include "RenderPass.h"
#include <d3d12.h>
#include <string>
#include <wrl.h>

class ShadowMap;
class MeshRenderer;
class SkyRenderer;
class GridRenderer;
class ParticleRenderer;
class PostProcessRenderer;
class SSAORenderer;
class ImGuiLayer;

// Pass 1: Render depth-only shadow map from the light's perspective.
class ShadowPass : public RenderPass {
public:
  ShadowPass(ShadowMap &shadow, MeshRenderer &mesh)
      : m_shadow(shadow), m_mesh(mesh) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  ShadowMap &m_shadow;
  MeshRenderer &m_mesh;
};

// Pass 2: Clear backbuffer + depth, draw HDRI sky background.
class SkyPass : public RenderPass {
public:
  explicit SkyPass(SkyRenderer &sky) : m_sky(sky) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  SkyRenderer &m_sky;
};

// Pass 3: Draw opaque geometry (grid/axes + lit meshes with shadows) [FORWARD].
class OpaquePass : public RenderPass {
public:
  OpaquePass(GridRenderer &grid, MeshRenderer &mesh, ShadowMap &shadow)
      : m_grid(grid), m_mesh(mesh), m_shadow(shadow) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  GridRenderer &m_grid;
  MeshRenderer &m_mesh;
  ShadowMap &m_shadow;
};

// Pass 3a: G-buffer fill (Phase 12.1 — Deferred Rendering).
class GBufferPass : public RenderPass {
public:
  GBufferPass(MeshRenderer &mesh) : m_mesh(mesh) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  MeshRenderer &m_mesh;
};

// Pass 3b: Deferred lighting — fullscreen pass (Phase 12.1).
class DeferredLightingPass : public RenderPass {
public:
  DeferredLightingPass(ShadowMap &shadow, MeshRenderer &mesh)
      : m_shadow(shadow), m_mesh(mesh) {}
  void Execute(DxContext &dx, const FrameData &frame) override;
  std::string ReloadShaders(DxContext &dx);

private:
  ShadowMap &m_shadow;
  MeshRenderer &m_mesh;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
  void CreatePipelineOnce(DxContext &dx);
};

// Pass 3c: Grid/axes draw (after deferred lighting, into HDR).
class GridPass : public RenderPass {
public:
  GridPass(GridRenderer &grid) : m_grid(grid) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  GridRenderer &m_grid;
};

// Pass 4: Draw transparent/additive geometry (particles).
class TransparentPass : public RenderPass {
public:
  explicit TransparentPass(ParticleRenderer &particles)
      : m_particles(particles) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  ParticleRenderer &m_particles;
};

// Pass 5: SSAO (generation + bilateral blur).
class SSAOPass : public RenderPass {
public:
  explicit SSAOPass(SSAORenderer &ssao) : m_ssao(ssao) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  SSAORenderer &m_ssao;
};

// Pass 6: Bloom (downsample + upsample).
class BloomPass : public RenderPass {
public:
  explicit BloomPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 6: Tonemapping (HDR + bloom -> LDR).
class TonemapPass : public RenderPass {
public:
  explicit TonemapPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 7: FXAA (LDR -> backbuffer).
class FXAAPass : public RenderPass {
public:
  explicit FXAAPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 8: Depth of Field (Phase 10.6).
class DOFPass : public RenderPass {
public:
  explicit DOFPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 9: Velocity generation (Phase 10.5 — Motion Blur).
class VelocityGenPass : public RenderPass {
public:
  explicit VelocityGenPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 9: Motion blur (Phase 10.5).
class MotionBlurPass : public RenderPass {
public:
  explicit MotionBlurPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Pass 10: TAA resolve (Phase 10.4).
class TAAPass : public RenderPass {
public:
  explicit TAAPass(PostProcessRenderer &pp) : m_pp(pp) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  PostProcessRenderer &m_pp;
};

// Highlight pass: wireframe overlay for selected entities (editor).
class HighlightPass : public RenderPass {
public:
  explicit HighlightPass(MeshRenderer &mesh) : m_mesh(mesh) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  MeshRenderer &m_mesh;
};

// Pass 11: Draw debug UI (ImGui overlay).
class UIPass : public RenderPass {
public:
  explicit UIPass(ImGuiLayer &imgui) : m_imgui(imgui) {}
  void Execute(DxContext &dx, const FrameData &frame) override;

private:
  ImGuiLayer &m_imgui;
};
