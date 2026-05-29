// ======================================
// File: ProceduralMesh.cpp
// Purpose: Procedural geometry generation.
// ======================================

#include "ProceduralMesh.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ProceduralMesh {

// Helper: push a MeshVertex.
static void PushVert(LoadedMesh &m, float px, float py, float pz, float nx,
                     float ny, float nz, float u, float v, float tx, float ty,
                     float tz, float tw) {
  MeshVertex vtx{};
  vtx.pos[0] = px;
  vtx.pos[1] = py;
  vtx.pos[2] = pz;
  vtx.normal[0] = nx;
  vtx.normal[1] = ny;
  vtx.normal[2] = nz;
  vtx.uv[0] = u;
  vtx.uv[1] = v;
  vtx.tangent[0] = tx;
  vtx.tangent[1] = ty;
  vtx.tangent[2] = tz;
  vtx.tangent[3] = tw;
  vtx.boneIndices[0] = vtx.boneIndices[1] = vtx.boneIndices[2] = vtx.boneIndices[3] = 0;
  vtx.boneWeights[0] = vtx.boneWeights[1] = vtx.boneWeights[2] = vtx.boneWeights[3] = 0.0f;
  m.vertices.push_back(vtx);
}

// Helper: push a triangle (3 uint32_t indices).
static void PushTri(LoadedMesh &m, uint32_t a, uint32_t b, uint32_t c) {
  m.indices.push_back(a);
  m.indices.push_back(b);
  m.indices.push_back(c);
}

LoadedMesh CreateCube(float size) {
  LoadedMesh m;
  const float h = size * 0.5f;

  // 6 faces, 4 verts each = 24 verts, 36 indices.
  // Face order: +Y (top), -Y (bottom), +Z (front), -Z (back), +X (right), -X
  // (left)

  struct FaceData {
    float nx, ny, nz;
    float tx, ty, tz;
    float p[4][3]; // 4 corners (CCW from outside)
  };

  // clang-format off
  FaceData faces[6] = {
    // +Y (top) — normal up, tangent +X
    {0, 1, 0,  1, 0, 0, {{-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}}},
    // -Y (bottom) — normal down, tangent +X
    {0, -1, 0, 1, 0, 0, {{-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}}},
    // +Z (front) — normal forward, tangent +X
    {0, 0, 1,  1, 0, 0, {{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}},
    // -Z (back) — normal backward, tangent -X
    {0, 0, -1, -1, 0, 0, {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}},
    // +X (right) — normal right, tangent +Z
    {1, 0, 0,  0, 0, -1, {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}}},
    // -X (left) — normal left, tangent -Z
    {-1, 0, 0, 0, 0, 1, {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}},
  };
  // clang-format on

  float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

  for (int f = 0; f < 6; ++f) {
    auto &fd = faces[f];
    auto base = static_cast<uint32_t>(m.vertices.size());
    for (int v = 0; v < 4; ++v) {
      PushVert(m, fd.p[v][0], fd.p[v][1], fd.p[v][2], fd.nx, fd.ny, fd.nz,
               uvs[v][0], uvs[v][1], fd.tx, fd.ty, fd.tz, 1.0f);
    }
    PushTri(m, base, base + 1, base + 2);
    PushTri(m, base, base + 2, base + 3);
  }

  return m;
}

LoadedMesh CreatePlane(float width, float depth) {
  LoadedMesh m;
  const float hw = width * 0.5f;
  const float hd = depth * 0.5f;

  // Normal up (+Y), tangent +X.
  PushVert(m, -hw, 0, -hd, 0, 1, 0, 0, 1, 1, 0, 0, 1);
  PushVert(m, hw, 0, -hd, 0, 1, 0, 1, 1, 1, 0, 0, 1);
  PushVert(m, hw, 0, hd, 0, 1, 0, 1, 0, 1, 0, 0, 1);
  PushVert(m, -hw, 0, hd, 0, 1, 0, 0, 0, 1, 0, 0, 1);

  PushTri(m, 0, 2, 1);
  PushTri(m, 0, 3, 2);

  return m;
}

LoadedMesh CreateCylinder(float radius, float height, uint32_t segments) {
  LoadedMesh m;
  const float halfH = height * 0.5f;

  // Side vertices: 2 rings (top and bottom) × (segments+1) verts.
  for (uint32_t i = 0; i <= segments; ++i) {
    float angle =
        static_cast<float>(i) / static_cast<float>(segments) * 2.0f *
        static_cast<float>(M_PI);
    float cx = cosf(angle);
    float cz = sinf(angle);
    float u = static_cast<float>(i) / static_cast<float>(segments);

    // Normal is radial (no Y component for sides).
    // Tangent follows the circumference direction.
    float tx = -sinf(angle);
    float tz = cosf(angle);

    // Bottom vertex
    PushVert(m, cx * radius, -halfH, cz * radius, cx, 0, cz, u, 1.0f, tx, 0,
             tz, 1.0f);
    // Top vertex
    PushVert(m, cx * radius, halfH, cz * radius, cx, 0, cz, u, 0.0f, tx, 0,
             tz, 1.0f);
  }

  // Side indices (CW winding for DX front-face).
  for (uint32_t i = 0; i < segments; ++i) {
    auto b = static_cast<uint32_t>(i * 2);
    PushTri(m, b, b + 3, b + 2);
    PushTri(m, b, b + 1, b + 3);
  }

  // Top cap (CW when viewed from above).
  {
    auto centerIdx = static_cast<uint32_t>(m.vertices.size());
    PushVert(m, 0, halfH, 0, 0, 1, 0, 0.5f, 0.5f, 1, 0, 0, 1);
    for (uint32_t i = 0; i <= segments; ++i) {
      float angle =
          static_cast<float>(i) / static_cast<float>(segments) * 2.0f *
          static_cast<float>(M_PI);
      float cx = cosf(angle);
      float cz = sinf(angle);
      PushVert(m, cx * radius, halfH, cz * radius, 0, 1, 0,
               cx * 0.5f + 0.5f, cz * 0.5f + 0.5f, 1, 0, 0, 1);
    }
    for (uint32_t i = 0; i < segments; ++i) {
      PushTri(m, centerIdx, static_cast<uint32_t>(centerIdx + 2 + i),
              static_cast<uint32_t>(centerIdx + 1 + i));
    }
  }

  // Bottom cap (CW when viewed from below).
  {
    auto centerIdx = static_cast<uint32_t>(m.vertices.size());
    PushVert(m, 0, -halfH, 0, 0, -1, 0, 0.5f, 0.5f, 1, 0, 0, 1);
    for (uint32_t i = 0; i <= segments; ++i) {
      float angle =
          static_cast<float>(i) / static_cast<float>(segments) * 2.0f *
          static_cast<float>(M_PI);
      float cx = cosf(angle);
      float cz = sinf(angle);
      PushVert(m, cx * radius, -halfH, cz * radius, 0, -1, 0,
               cx * 0.5f + 0.5f, cz * 0.5f + 0.5f, 1, 0, 0, 1);
    }
    for (uint32_t i = 0; i < segments; ++i) {
      PushTri(m, centerIdx, static_cast<uint32_t>(centerIdx + 1 + i),
              static_cast<uint32_t>(centerIdx + 2 + i));
    }
  }

  return m;
}

LoadedMesh CreateCone(float radius, float height, uint32_t segments) {
  LoadedMesh m;

  // The cone tip is at (0, height, 0), base center at (0, 0, 0).
  const float slope =
      radius / height; // for normal calculation: ny = radius/height normalized

  // Side vertices: tip + base ring.
  // Each segment gets its own tip vertex (different normal per face).
  for (uint32_t i = 0; i < segments; ++i) {
    float a0 = static_cast<float>(i) / static_cast<float>(segments) * 2.0f *
               static_cast<float>(M_PI);
    float a1 = static_cast<float>(i + 1) / static_cast<float>(segments) *
               2.0f * static_cast<float>(M_PI);
    float midA = (a0 + a1) * 0.5f;

    float cx0 = cosf(a0), cz0 = sinf(a0);
    float cx1 = cosf(a1), cz1 = sinf(a1);
    float cxM = cosf(midA), czM = sinf(midA);

    // Normal for this face: radial outward + upward component.
    float len = sqrtf(1.0f + slope * slope);
    float nx = cxM / len;
    float ny = slope / len;
    float nz = czM / len;

    // Tangent along the base edge.
    float tx = -sinf(midA);
    float tz = cosf(midA);

    auto base = static_cast<uint32_t>(m.vertices.size());

    // Tip
    PushVert(m, 0, height, 0, nx, ny, nz, 0.5f, 0.0f, tx, 0, tz, 1.0f);
    // Base left
    PushVert(m, cx0 * radius, 0, cz0 * radius, nx, ny, nz,
             static_cast<float>(i) / static_cast<float>(segments), 1.0f, tx, 0,
             tz, 1.0f);
    // Base right
    PushVert(m, cx1 * radius, 0, cz1 * radius, nx, ny, nz,
             static_cast<float>(i + 1) / static_cast<float>(segments), 1.0f,
             tx, 0, tz, 1.0f);

    PushTri(m, base, base + 2, base + 1);
  }

  // Bottom cap (CW when viewed from below).
  {
    auto centerIdx = static_cast<uint32_t>(m.vertices.size());
    PushVert(m, 0, 0, 0, 0, -1, 0, 0.5f, 0.5f, 1, 0, 0, 1);
    for (uint32_t i = 0; i <= segments; ++i) {
      float angle =
          static_cast<float>(i) / static_cast<float>(segments) * 2.0f *
          static_cast<float>(M_PI);
      float cx = cosf(angle);
      float cz = sinf(angle);
      PushVert(m, cx * radius, 0, cz * radius, 0, -1, 0, cx * 0.5f + 0.5f,
               cz * 0.5f + 0.5f, 1, 0, 0, 1);
    }
    for (uint32_t i = 0; i < segments; ++i) {
      PushTri(m, centerIdx, static_cast<uint32_t>(centerIdx + 1 + i),
              static_cast<uint32_t>(centerIdx + 2 + i));
    }
  }

  return m;
}

LoadedMesh CreateSphere(float radius, uint32_t rings, uint32_t segments) {
  LoadedMesh m;

  // Generate vertices: (rings+1) latitude lines × (segments+1) longitude
  // points.
  for (uint32_t r = 0; r <= rings; ++r) {
    float phi =
        static_cast<float>(r) / static_cast<float>(rings) *
        static_cast<float>(M_PI);
    float sinPhi = sinf(phi);
    float cosPhi = cosf(phi);
    float v = static_cast<float>(r) / static_cast<float>(rings);

    for (uint32_t s = 0; s <= segments; ++s) {
      float theta =
          static_cast<float>(s) / static_cast<float>(segments) * 2.0f *
          static_cast<float>(M_PI);
      float sinTheta = sinf(theta);
      float cosTheta = cosf(theta);
      float u = static_cast<float>(s) / static_cast<float>(segments);

      float nx = sinPhi * cosTheta;
      float ny = cosPhi;
      float nz = sinPhi * sinTheta;

      // Tangent: derivative of position w.r.t. theta (longitude direction).
      float tx = -sinTheta;
      float tz = cosTheta;

      PushVert(m, nx * radius, ny * radius, nz * radius, nx, ny, nz, u, v, tx,
               0, tz, 1.0f);
    }
  }

  // Indices.
  uint32_t cols = segments + 1;
  for (uint32_t r = 0; r < rings; ++r) {
    for (uint32_t s = 0; s < segments; ++s) {
      auto tl = static_cast<uint32_t>(r * cols + s);
      auto tr = static_cast<uint32_t>(r * cols + s + 1);
      auto bl = static_cast<uint32_t>((r + 1) * cols + s);
      auto br = static_cast<uint32_t>((r + 1) * cols + s + 1);

      PushTri(m, tl, tr, bl);
      PushTri(m, tr, br, bl);
    }
  }

  return m;
}

} // namespace ProceduralMesh
