// ======================================
// File: ProceduralMesh.h
// Purpose: Generate procedural geometry as LoadedMesh structs for the engine
//          pipeline. All meshes have correct MeshVertex format (pos, normal,
//          uv, tangent) and uint32_t indices.
// ======================================

#pragma once

#include "GltfLoader.h" // MeshVertex, LoadedMesh

#include <cstdint>

namespace ProceduralMesh {

// Unit cube centered at origin, size = edge length.
LoadedMesh CreateCube(float size = 1.0f);

// Flat quad on the XZ plane, centered at origin.
LoadedMesh CreatePlane(float width = 1.0f, float depth = 1.0f);

// 水面など、頂点変形を行うための分割済み XZ 平面。
LoadedMesh CreateTessellatedPlane(float width = 1.0f, float depth = 1.0f,
                                  uint32_t xSegments = 16,
                                  uint32_t zSegments = 16);

// Vertical cylinder along Y axis, centered at origin.
LoadedMesh CreateCylinder(float radius = 0.5f, float height = 1.0f,
                          uint32_t segments = 16);

// Cone pointing up along Y axis, base centered at origin.
LoadedMesh CreateCone(float radius = 0.5f, float height = 1.0f,
                      uint32_t segments = 16);

// UV sphere centered at origin.
LoadedMesh CreateSphere(float radius = 0.5f, uint32_t rings = 12,
                        uint32_t segments = 24);

// 地面の結界やシールド向けの上半球。底面キャップは生成しない。
LoadedMesh CreateHemisphere(float radius = 0.5f, uint32_t rings = 12,
                            uint32_t segments = 24);

} // namespace ProceduralMesh
