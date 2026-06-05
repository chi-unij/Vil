#pragma once

#include <DirectXMath.h>
#include <vector>

namespace CollisionSystem {

struct Aabb {
  DirectX::XMFLOAT3 min = {0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 max = {0.0f, 0.0f, 0.0f};
};

struct Capsule {
  float radius = 0.45f;
  float halfHeight = 0.85f;
};

enum class ShapeType : int {
  Box = 0,
  Circle = 1,
  Triangle = 2,
};

struct Collider {
  ShapeType shape = ShapeType::Box;
  DirectX::XMFLOAT3 center = {0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 size = {1.0f, 1.0f, 1.0f};
  float yawRadians = 0.0f;
  bool enabled = true;
};

struct MeshTriangle {
  DirectX::XMFLOAT2 a = {0.0f, 0.0f};
  DirectX::XMFLOAT2 b = {0.0f, 0.0f};
  DirectX::XMFLOAT2 c = {0.0f, 0.0f};
};

DirectX::XMFLOAT3 ResolveCapsuleAgainstAabbs(
    const DirectX::XMFLOAT3 &desiredPosition, const Capsule &capsule,
    const std::vector<Aabb> &aabbs, float worldHalfExtentMeters);

DirectX::XMFLOAT3 ResolveCapsuleAgainstColliders(
    const DirectX::XMFLOAT3 &desiredPosition, const Capsule &capsule,
    const std::vector<Collider> &colliders, float worldHalfExtentMeters);

DirectX::XMFLOAT3 ResolveCapsuleAgainstCollidersAndMesh(
    const DirectX::XMFLOAT3 &desiredPosition, const Capsule &capsule,
    const std::vector<Collider> &colliders,
    const std::vector<MeshTriangle> &meshTriangles,
    float worldHalfExtentMeters);

} // namespace CollisionSystem
