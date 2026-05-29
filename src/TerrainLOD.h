// ======================================
// File: TerrainLOD.h
// Purpose: Chunked terrain with CPU-side Perlin noise heightmap, per-chunk
//          distance-based LOD selection, and frustum culling. Phase 12.3.
//          Each chunk has unique geometry (height-displaced vertices) so
//          collision data matches visuals.
// ======================================
#pragma once

#include "GltfLoader.h" // LoadedMesh, MaterialImages, Material
#include "RenderPass.h"  // RenderItem

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>

class DxContext;
class Camera;

// ---- Configuration ----
struct TerrainLODConfig {
  float worldSize       = 512.0f;           // total extent in X and Z
  int   chunksPerAxis   = 8;                // 8x8 = 64 chunks
  float lodDist[3]      = {80.0f, 200.0f, 1e9f}; // max distance for each LOD
  int   subdivisions[3] = {32, 16, 8};      // quads per chunk axis per LOD
  float uvTilesPerChunk = 4.0f;             // UV tiles across one chunk

  // Heightmap noise parameters
  float heightScale     = 40.0f;            // max height range (0 to heightScale)
  float noiseFrequency  = 0.01f;            // base Perlin frequency
  int   noiseOctaves    = 5;                // fBm octave count
  float noiseLacunarity = 2.0f;             // frequency multiplier per octave
  float noisePersistence = 0.5f;            // amplitude decay per octave
};

// Per-chunk metadata.
struct TerrainChunk {
  DirectX::XMFLOAT3 center;    // world-space center (includes Y midpoint)
  float halfExtent;             // half of chunk side length (XZ)
  DirectX::XMFLOAT3 aabbMin;
  DirectX::XMFLOAT3 aabbMax;
  DirectX::XMMATRIX world;     // translation-only transform (XZ offset, Y=0)
  std::array<uint32_t, 3> lodMeshIds = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
};

class TerrainLOD {
public:
  // Build heightmap, generate per-chunk meshes, register with MeshRenderer.
  void Initialize(DxContext &dx, const MaterialImages &images,
                  const Material &material,
                  const TerrainLODConfig &cfg = {});

  // Per-frame: frustum cull + LOD select, append RenderItems to outItems.
  void Update(const Camera &cam, std::vector<RenderItem> &outItems);

  // ImGui debug panel.
  void DrawDebugUI();

  // CPU-side height query (bilinear interpolated). For collision/gameplay.
  float GetHeightAt(float worldX, float worldZ) const;

  // Access any valid mesh ID (for material editing — all share same material).
  uint32_t GetAnyMeshId() const;
  bool     IsInitialized() const { return !m_chunks.empty() && m_chunks[0].lodMeshIds[0] != UINT32_MAX; }

private:
  // Generate a heightmap-displaced chunk mesh at the given LOD.
  LoadedMesh GenerateHeightChunkMesh(int quadsPerAxis, float chunkSize,
                                     float uvTiles,
                                     float chunkWorldX, float chunkWorldZ) const;

  // Sample the heightmap at world coordinates (clamped, no interpolation).
  float SampleHeightmap(float worldX, float worldZ) const;

  using FrustumPlanes = std::array<DirectX::XMVECTOR, 6>;
  static FrustumPlanes ExtractFrustumPlanes(DirectX::FXMMATRIX viewProj);
  static bool IsAABBVisible(const DirectX::XMFLOAT3 &aabbMin,
                            const DirectX::XMFLOAT3 &aabbMax,
                            const FrustumPlanes &planes);

private:
  TerrainLODConfig            m_cfg;
  std::vector<TerrainChunk>   m_chunks;

  // Full-resolution heightmap covering the entire terrain.
  std::vector<float>          m_heightmap;
  int                         m_heightmapRes = 0; // samples per axis

  // Debug state.
  bool m_frustumCullEnabled = false;
  int m_visibleCount[3] = {};
  int m_culledCount     = 0;
};
