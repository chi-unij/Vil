// ======================================
// File: TerrainLOD.cpp
// Purpose: Chunked terrain with CPU-side Perlin noise heightmap (Phase 12.3).
//          Generates per-chunk unique geometry at 3 LOD levels with proper
//          normals computed from height gradients.
// ======================================

#include "TerrainLOD.h"
#include "Camera.h"
#include "DxContext.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <imgui.h>

using namespace DirectX;

// ============================================================
// Perlin noise (classic 2D, Ken Perlin's improved version)
// ============================================================

namespace {

// Permutation table (doubled to avoid wrapping).
static const int kPerm[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
    // Repeat
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
};

static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float Lerp(float a, float b, float t) { return a + t * (b - a); }

static float Grad(int hash, float x, float y) {
  int h = hash & 7;
  float u = h < 4 ? x : y;
  float v = h < 4 ? y : x;
  return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

static float PerlinNoise2D(float x, float y) {
  int xi = static_cast<int>(std::floor(x)) & 255;
  int yi = static_cast<int>(std::floor(y)) & 255;
  float xf = x - std::floor(x);
  float yf = y - std::floor(y);
  float u = Fade(xf);
  float v = Fade(yf);

  int aa = kPerm[kPerm[xi] + yi];
  int ab = kPerm[kPerm[xi] + yi + 1];
  int ba = kPerm[kPerm[xi + 1] + yi];
  int bb = kPerm[kPerm[xi + 1] + yi + 1];

  float x1 = Lerp(Grad(aa, xf, yf), Grad(ba, xf - 1.0f, yf), u);
  float x2 = Lerp(Grad(ab, xf, yf - 1.0f), Grad(bb, xf - 1.0f, yf - 1.0f), u);
  return Lerp(x1, x2, v);
}

// Fractal Brownian Motion: stack octaves of Perlin noise.
static float FBm(float x, float y, int octaves, float lacunarity,
                 float persistence) {
  float value = 0.0f;
  float amplitude = 1.0f;
  float frequency = 1.0f;
  float maxAmp = 0.0f;

  for (int i = 0; i < octaves; ++i) {
    value += amplitude * PerlinNoise2D(x * frequency, y * frequency);
    maxAmp += amplitude;
    amplitude *= persistence;
    frequency *= lacunarity;
  }

  // Normalize to [0, 1].
  return (value / maxAmp) * 0.5f + 0.5f;
}

} // anonymous namespace

// ============================================================
// Heightmap sampling helpers
// ============================================================

float TerrainLOD::SampleHeightmap(float worldX, float worldZ) const {
  if (m_heightmapRes <= 0)
    return 0.0f;

  float halfWorld = m_cfg.worldSize * 0.5f;
  // Map world coords to heightmap grid coords [0, res-1].
  float u = (worldX + halfWorld) / m_cfg.worldSize *
            static_cast<float>(m_heightmapRes - 1);
  float v = (worldZ + halfWorld) / m_cfg.worldSize *
            static_cast<float>(m_heightmapRes - 1);

  int x0 = static_cast<int>(std::floor(u));
  int z0 = static_cast<int>(std::floor(v));
  x0 = std::clamp(x0, 0, m_heightmapRes - 1);
  z0 = std::clamp(z0, 0, m_heightmapRes - 1);

  return m_heightmap[static_cast<size_t>(z0) * m_heightmapRes + x0];
}

float TerrainLOD::GetHeightAt(float worldX, float worldZ) const {
  if (m_heightmapRes <= 1)
    return 0.0f;

  float halfWorld = m_cfg.worldSize * 0.5f;
  float u = (worldX + halfWorld) / m_cfg.worldSize *
            static_cast<float>(m_heightmapRes - 1);
  float v = (worldZ + halfWorld) / m_cfg.worldSize *
            static_cast<float>(m_heightmapRes - 1);

  // Bilinear interpolation.
  int x0 = static_cast<int>(std::floor(u));
  int z0 = static_cast<int>(std::floor(v));
  int x1 = x0 + 1;
  int z1 = z0 + 1;
  x0 = std::clamp(x0, 0, m_heightmapRes - 1);
  z0 = std::clamp(z0, 0, m_heightmapRes - 1);
  x1 = std::clamp(x1, 0, m_heightmapRes - 1);
  z1 = std::clamp(z1, 0, m_heightmapRes - 1);

  float fx = u - std::floor(u);
  float fz = v - std::floor(v);

  float h00 = m_heightmap[static_cast<size_t>(z0) * m_heightmapRes + x0];
  float h10 = m_heightmap[static_cast<size_t>(z0) * m_heightmapRes + x1];
  float h01 = m_heightmap[static_cast<size_t>(z1) * m_heightmapRes + x0];
  float h11 = m_heightmap[static_cast<size_t>(z1) * m_heightmapRes + x1];

  float h0 = h00 + fx * (h10 - h00);
  float h1 = h01 + fx * (h11 - h01);
  return h0 + fz * (h1 - h0);
}

// ============================================================
// Mesh generation (with height displacement + computed normals)
// ============================================================

LoadedMesh TerrainLOD::GenerateHeightChunkMesh(int quadsPerAxis,
                                                float chunkSize,
                                                float uvTiles,
                                                float chunkWorldX,
                                                float chunkWorldZ) const {
  LoadedMesh mesh;
  const int N = quadsPerAxis;
  const int vertsPerAxis = N + 1;
  const float step = chunkSize / static_cast<float>(N);
  const float half = chunkSize * 0.5f;
  const float uvStep = uvTiles / static_cast<float>(N);
  // Small delta for finite-difference normal computation.
  const float delta = m_cfg.worldSize / static_cast<float>(m_heightmapRes - 1);

  mesh.vertices.reserve(static_cast<size_t>(vertsPerAxis * vertsPerAxis));
  mesh.indices.reserve(static_cast<size_t>(N * N * 6));

  for (int r = 0; r < vertsPerAxis; ++r) {
    for (int c = 0; c < vertsPerAxis; ++c) {
      float localX = -half + c * step;
      float localZ = -half + r * step;
      float worldX = chunkWorldX + localX;
      float worldZ = chunkWorldZ + localZ;

      float height = GetHeightAt(worldX, worldZ);

      // Compute normal from finite differences of the heightmap.
      float hL = GetHeightAt(worldX - delta, worldZ);
      float hR = GetHeightAt(worldX + delta, worldZ);
      float hD = GetHeightAt(worldX, worldZ - delta);
      float hU = GetHeightAt(worldX, worldZ + delta);

      // Normal = normalize(cross(tangentX, tangentZ))
      // tangentX = (2*delta, hR - hL, 0), tangentZ = (0, hU - hD, 2*delta)
      // cross = (-(hR-hL)*2*delta ... simplified:
      float nx = -(hR - hL);
      float ny = 2.0f * delta;
      float nz = -(hU - hD);
      float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-6f) {
        nx /= len;
        ny /= len;
        nz /= len;
      } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
      }

      // Tangent along X direction (accounting for slope).
      float tx = 2.0f * delta;
      float ty = hR - hL;
      float tz = 0.0f;
      float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
      if (tlen > 1e-6f) {
        tx /= tlen;
        ty /= tlen;
      } else {
        tx = 1.0f;
        ty = 0.0f;
      }

      MeshVertex v{};
      v.pos[0] = localX;
      v.pos[1] = height;
      v.pos[2] = localZ;
      v.normal[0] = nx;
      v.normal[1] = ny;
      v.normal[2] = nz;
      v.uv[0] = c * uvStep;
      v.uv[1] = r * uvStep;
      v.tangent[0] = tx;
      v.tangent[1] = ty;
      v.tangent[2] = tz;
      v.tangent[3] = 1.0f;

      mesh.vertices.push_back(v);
    }
  }

  // Indices (same winding as before).
  for (int r = 0; r < N; ++r) {
    for (int c = 0; c < N; ++c) {
      uint16_t tl = static_cast<uint16_t>(r * vertsPerAxis + c);
      uint16_t tr = tl + 1;
      uint16_t bl = static_cast<uint16_t>((r + 1) * vertsPerAxis + c);
      uint16_t br = bl + 1;

      // Winding: {tl, br, tr}, {tl, bl, br} — matches MakeTiledPlaneXZ LH winding.
      mesh.indices.push_back(tl);
      mesh.indices.push_back(br);
      mesh.indices.push_back(tr);

      mesh.indices.push_back(tl);
      mesh.indices.push_back(bl);
      mesh.indices.push_back(br);
    }
  }

  return mesh;
}

// ============================================================
// Initialization
// ============================================================

void TerrainLOD::Initialize(DxContext &dx, const MaterialImages &images,
                            const Material &material,
                            const TerrainLODConfig &cfg) {
  m_cfg = cfg;
  const float chunkSize =
      m_cfg.worldSize / static_cast<float>(m_cfg.chunksPerAxis);
  const float halfWorld = m_cfg.worldSize * 0.5f;

  // Step 1: Generate full-resolution heightmap via Perlin fBm.
  m_heightmapRes = m_cfg.chunksPerAxis * m_cfg.subdivisions[0] + 1; // e.g., 257
  m_heightmap.resize(static_cast<size_t>(m_heightmapRes) * m_heightmapRes);

  for (int z = 0; z < m_heightmapRes; ++z) {
    for (int x = 0; x < m_heightmapRes; ++x) {
      float worldX =
          -halfWorld +
          static_cast<float>(x) / static_cast<float>(m_heightmapRes - 1) *
              m_cfg.worldSize;
      float worldZ =
          -halfWorld +
          static_cast<float>(z) / static_cast<float>(m_heightmapRes - 1) *
              m_cfg.worldSize;

      float h = FBm(worldX * m_cfg.noiseFrequency,
                     worldZ * m_cfg.noiseFrequency, m_cfg.noiseOctaves,
                     m_cfg.noiseLacunarity, m_cfg.noisePersistence);
      m_heightmap[static_cast<size_t>(z) * m_heightmapRes + x] =
          h * m_cfg.heightScale;
    }
  }

  // Step 2: Build chunk grid and per-chunk meshes.
  m_chunks.clear();
  m_chunks.reserve(
      static_cast<size_t>(m_cfg.chunksPerAxis * m_cfg.chunksPerAxis));

  for (int iz = 0; iz < m_cfg.chunksPerAxis; ++iz) {
    for (int ix = 0; ix < m_cfg.chunksPerAxis; ++ix) {
      TerrainChunk chunk{};
      float cx = -halfWorld + (ix + 0.5f) * chunkSize;
      float cz = -halfWorld + (iz + 0.5f) * chunkSize;
      float h = chunkSize * 0.5f;

      // Compute AABB Y extent from heightmap samples in this chunk.
      float yMin = 1e9f;
      float yMax = -1e9f;
      int startX = ix * m_cfg.subdivisions[0];
      int startZ = iz * m_cfg.subdivisions[0];
      int endX = startX + m_cfg.subdivisions[0];
      int endZ = startZ + m_cfg.subdivisions[0];
      for (int sz = startZ; sz <= endZ; ++sz) {
        for (int sx = startX; sx <= endX; ++sx) {
          float val = m_heightmap[static_cast<size_t>(sz) * m_heightmapRes + sx];
          yMin = std::min(yMin, val);
          yMax = std::max(yMax, val);
        }
      }

      chunk.center = {cx, (yMin + yMax) * 0.5f, cz};
      chunk.halfExtent = h;
      chunk.aabbMin = {cx - h, yMin - 0.1f, cz - h};
      chunk.aabbMax = {cx + h, yMax + 0.1f, cz + h};
      // World transform: XZ translation only. Y=0 because height is baked
      // into vertex positions (local space).
      chunk.world = XMMatrixTranslation(cx, 0.0f, cz);

      // Generate meshes for all 3 LOD levels.
      for (int lod = 0; lod < 3; ++lod) {
        LoadedMesh mesh = GenerateHeightChunkMesh(
            m_cfg.subdivisions[lod], chunkSize, m_cfg.uvTilesPerChunk, cx, cz);
        chunk.lodMeshIds[lod] = dx.CreateMeshResources(mesh, images, material);
      }

      m_chunks.push_back(chunk);
    }
  }
}

// ============================================================
// Frustum extraction (Gribb-Hartmann method)
// ============================================================

TerrainLOD::FrustumPlanes
TerrainLOD::ExtractFrustumPlanes(FXMMATRIX viewProj) {
  XMFLOAT4X4 m;
  XMStoreFloat4x4(&m, viewProj);

  XMVECTOR r0 = XMVectorSet(m._11, m._12, m._13, m._14);
  XMVECTOR r1 = XMVectorSet(m._21, m._22, m._23, m._24);
  XMVECTOR r2 = XMVectorSet(m._31, m._32, m._33, m._34);
  XMVECTOR r3 = XMVectorSet(m._41, m._42, m._43, m._44);

  FrustumPlanes planes;
  planes[0] = XMPlaneNormalize(XMVectorAdd(r3, r0));      // left
  planes[1] = XMPlaneNormalize(XMVectorSubtract(r3, r0)); // right
  planes[2] = XMPlaneNormalize(XMVectorAdd(r3, r1));      // bottom
  planes[3] = XMPlaneNormalize(XMVectorSubtract(r3, r1)); // top
  planes[4] = XMPlaneNormalize(r2);                        // near (LH)
  planes[5] = XMPlaneNormalize(XMVectorSubtract(r3, r2)); // far

  return planes;
}

// ============================================================
// AABB frustum test (positive-vertex method)
// ============================================================

bool TerrainLOD::IsAABBVisible(const XMFLOAT3 &aabbMin,
                               const XMFLOAT3 &aabbMax,
                               const FrustumPlanes &planes) {
  for (int i = 0; i < 6; ++i) {
    XMFLOAT4 plane;
    XMStoreFloat4(&plane, planes[i]);

    float px = (plane.x >= 0.0f) ? aabbMax.x : aabbMin.x;
    float py = (plane.y >= 0.0f) ? aabbMax.y : aabbMin.y;
    float pz = (plane.z >= 0.0f) ? aabbMax.z : aabbMin.z;

    float dot = plane.x * px + plane.y * py + plane.z * pz + plane.w;
    if (dot < 0.0f)
      return false;
  }
  return true;
}

// ============================================================
// Per-frame update
// ============================================================

void TerrainLOD::Update(const Camera &cam,
                        std::vector<RenderItem> &outItems) {
  XMMATRIX vp = XMMatrixMultiply(cam.View(), cam.Proj());
  FrustumPlanes planes = ExtractFrustumPlanes(vp);
  XMFLOAT3 camPos = cam.GetPosition();

  m_visibleCount[0] = m_visibleCount[1] = m_visibleCount[2] = 0;
  m_culledCount = 0;

  for (const auto &chunk : m_chunks) {
    if (m_frustumCullEnabled &&
        !IsAABBVisible(chunk.aabbMin, chunk.aabbMax, planes)) {
      ++m_culledCount;
      continue;
    }

    float ddx = camPos.x - chunk.center.x;
    float ddz = camPos.z - chunk.center.z;
    float dist = std::sqrt(ddx * ddx + ddz * ddz);

    int lod = 0;
    if (dist > m_cfg.lodDist[0])
      lod = 1;
    if (dist > m_cfg.lodDist[1])
      lod = 2;

    outItems.push_back({chunk.lodMeshIds[lod], chunk.world});
    ++m_visibleCount[lod];
  }
}

// ============================================================
// Public accessors
// ============================================================

uint32_t TerrainLOD::GetAnyMeshId() const {
  if (!m_chunks.empty())
    return m_chunks[0].lodMeshIds[0];
  return UINT32_MAX;
}

// ============================================================
// ImGui debug panel
// ============================================================

void TerrainLOD::DrawDebugUI() {
  ImGui::Begin("Terrain LOD");

  int totalVisible =
      m_visibleCount[0] + m_visibleCount[1] + m_visibleCount[2];
  ImGui::Text("Chunks: %d total, %d visible, %d culled",
              static_cast<int>(m_chunks.size()), totalVisible, m_culledCount);
  ImGui::Separator();

  ImGui::Text("LOD0 (%d quads): %d chunks", m_cfg.subdivisions[0],
              m_visibleCount[0]);
  ImGui::Text("LOD1 (%d quads): %d chunks", m_cfg.subdivisions[1],
              m_visibleCount[1]);
  ImGui::Text("LOD2 (%d quads): %d chunks", m_cfg.subdivisions[2],
              m_visibleCount[2]);
  ImGui::Separator();

  ImGui::SliderFloat("LOD0 dist", &m_cfg.lodDist[0], 10.0f, 300.0f, "%.0f");
  ImGui::SliderFloat("LOD1 dist", &m_cfg.lodDist[1], 50.0f, 600.0f, "%.0f");

  if (m_cfg.lodDist[1] < m_cfg.lodDist[0])
    m_cfg.lodDist[1] = m_cfg.lodDist[0];

  ImGui::Separator();
  ImGui::Checkbox("Frustum Culling", &m_frustumCullEnabled);
  ImGui::Text("Heightmap: %dx%d, scale=%.0f", m_heightmapRes, m_heightmapRes,
              m_cfg.heightScale);

  ImGui::End();
}
