#include "game/CollisionSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace CollisionSystem {
namespace {

float Clamp(float value, float minValue, float maxValue) {
  return std::max(minValue, std::min(value, maxValue));
}

DirectX::XMFLOAT2 Rotate2(float x, float z, float radians) {
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  return {x * c - z * s, x * s + z * c};
}

float LengthSq(float x, float z) {
  return x * x + z * z;
}

bool ResolveCircleAgainstCircle(float &x, float &z, float radius,
                                const Collider &collider) {
  const float colliderRadius =
      std::max(0.01f, std::max(collider.size.x, collider.size.z) * 0.5f);
  float dx = x - collider.center.x;
  float dz = z - collider.center.z;
  const float minDist = radius + colliderRadius;
  const float distSq = LengthSq(dx, dz);
  if (distSq >= minDist * minDist)
    return false;

  if (distSq > 0.000001f) {
    const float dist = std::sqrt(distSq);
    const float push = minDist - dist;
    x += (dx / dist) * push;
    z += (dz / dist) * push;
  } else {
    z += minDist;
  }
  return true;
}

bool ResolveCircleAgainstAabb(float &x, float &z, float radius,
                              const Aabb &aabb) {
  const float nearestX = Clamp(x, aabb.min.x, aabb.max.x);
  const float nearestZ = Clamp(z, aabb.min.z, aabb.max.z);
  float dx = x - nearestX;
  float dz = z - nearestZ;
  const float distSq = dx * dx + dz * dz;
  const float radiusSq = radius * radius;

  if (distSq >= radiusSq)
    return false;

  if (distSq > 0.000001f) {
    const float dist = std::sqrt(distSq);
    const float push = radius - dist;
    x += (dx / dist) * push;
    z += (dz / dist) * push;
    return true;
  }

  const float pushLeft = std::abs(x - aabb.min.x);
  const float pushRight = std::abs(aabb.max.x - x);
  const float pushBack = std::abs(z - aabb.min.z);
  const float pushForward = std::abs(aabb.max.z - z);
  const float minPush = std::min(std::min(pushLeft, pushRight),
                                 std::min(pushBack, pushForward));

  if (minPush == pushLeft) {
    x = aabb.min.x - radius;
  } else if (minPush == pushRight) {
    x = aabb.max.x + radius;
  } else if (minPush == pushBack) {
    z = aabb.min.z - radius;
  } else {
    z = aabb.max.z + radius;
  }
  return true;
}

bool ResolveCircleAgainstBox(float &x, float &z, float radius,
                             const Collider &collider) {
  const XMFLOAT2 local =
      Rotate2(x - collider.center.x, z - collider.center.z,
              -collider.yawRadians);
  const Aabb localBox{{-collider.size.x * 0.5f, 0.0f,
                       -collider.size.z * 0.5f},
                      {collider.size.x * 0.5f, collider.size.y,
                       collider.size.z * 0.5f}};
  float localX = local.x;
  float localZ = local.y;
  if (!ResolveCircleAgainstAabb(localX, localZ, radius, localBox))
    return false;

  const XMFLOAT2 world =
      Rotate2(localX, localZ, collider.yawRadians);
  x = collider.center.x + world.x;
  z = collider.center.z + world.y;
  return true;
}

std::array<XMFLOAT2, 3> BuildTriangleVertices(const Collider &collider) {
  const float halfX = std::max(0.01f, collider.size.x * 0.5f);
  const float halfZ = std::max(0.01f, collider.size.z * 0.5f);
  std::array<XMFLOAT2, 3> local = {
      XMFLOAT2{0.0f, halfZ},
      XMFLOAT2{-halfX, -halfZ},
      XMFLOAT2{halfX, -halfZ},
  };

  for (XMFLOAT2 &p : local) {
    p = Rotate2(p.x, p.y, collider.yawRadians);
    p.x += collider.center.x;
    p.y += collider.center.z;
  }
  return local;
}

float Dot2(float ax, float az, float bx, float bz) {
  return ax * bx + az * bz;
}

XMFLOAT2 ClosestPointOnSegment(const XMFLOAT2 &p, const XMFLOAT2 &a,
                               const XMFLOAT2 &b) {
  const float abx = b.x - a.x;
  const float abz = b.y - a.y;
  const float denom = LengthSq(abx, abz);
  if (denom <= 0.000001f)
    return a;
  const float t = Clamp(Dot2(p.x - a.x, p.y - a.y, abx, abz) / denom,
                        0.0f, 1.0f);
  return {a.x + abx * t, a.y + abz * t};
}

bool PointInsideTriangle(const XMFLOAT2 &p,
                         const std::array<XMFLOAT2, 3> &tri) {
  bool hasNeg = false;
  bool hasPos = false;
  for (int i = 0; i < 3; ++i) {
    const XMFLOAT2 &a = tri[i];
    const XMFLOAT2 &b = tri[(i + 1) % 3];
    const float cross =
        (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    hasNeg |= cross < 0.0f;
    hasPos |= cross > 0.0f;
  }
  return !(hasNeg && hasPos);
}

bool ResolveCircleAgainstTrianglePoints(float &x, float &z, float radius,
                                        const std::array<XMFLOAT2, 3> &tri);

bool ResolveCircleAgainstTriangle(float &x, float &z, float radius,
                                  const Collider &collider) {
  const std::array<XMFLOAT2, 3> tri = BuildTriangleVertices(collider);
  return ResolveCircleAgainstTrianglePoints(x, z, radius, tri);
}

bool ResolveCircleAgainstTrianglePoints(float &x, float &z, float radius,
                                        const std::array<XMFLOAT2, 3> &tri) {
  const XMFLOAT2 p{x, z};
  const bool inside = PointInsideTriangle(p, tri);

  float bestDistSq = std::numeric_limits<float>::max();
  XMFLOAT2 bestPoint = tri[0];
  XMFLOAT2 bestA = tri[0];
  XMFLOAT2 bestB = tri[1];
  for (int i = 0; i < 3; ++i) {
    const XMFLOAT2 &a = tri[i];
    const XMFLOAT2 &b = tri[(i + 1) % 3];
    const XMFLOAT2 closest = ClosestPointOnSegment(p, a, b);
    const float distSq = LengthSq(p.x - closest.x, p.y - closest.y);
    if (distSq < bestDistSq) {
      bestDistSq = distSq;
      bestPoint = closest;
      bestA = a;
      bestB = b;
    }
  }

  if (!inside && bestDistSq >= radius * radius)
    return false;

  float nx = p.x - bestPoint.x;
  float nz = p.y - bestPoint.y;
  float distSq = LengthSq(nx, nz);
  float push = radius;
  if (distSq > 0.000001f) {
    const float dist = std::sqrt(distSq);
    nx /= dist;
    nz /= dist;
    push = inside ? (radius + dist) : (radius - dist);
  } else {
    const float ex = bestB.x - bestA.x;
    const float ez = bestB.y - bestA.y;
    nx = ez;
    nz = -ex;
    const float len = std::sqrt(LengthSq(nx, nz));
    if (len > 0.000001f) {
      nx /= len;
      nz /= len;
    } else {
      nx = 0.0f;
      nz = 1.0f;
    }
  }

  x += nx * push;
  z += nz * push;
  return true;
}

bool ResolveCircleAgainstMeshTriangle(float &x, float &z, float radius,
                                      const MeshTriangle &triangle) {
  const std::array<XMFLOAT2, 3> tri = {triangle.a, triangle.b, triangle.c};
  return ResolveCircleAgainstTrianglePoints(x, z, radius, tri);
}

bool ResolveCircleAgainstCollider(float &x, float &z, float radius,
                                  const Collider &collider) {
  if (!collider.enabled)
    return false;

  switch (collider.shape) {
  case ShapeType::Circle:
    return ResolveCircleAgainstCircle(x, z, radius, collider);
  case ShapeType::Triangle:
    return ResolveCircleAgainstTriangle(x, z, radius, collider);
  case ShapeType::Box:
  default:
    return ResolveCircleAgainstBox(x, z, radius, collider);
  }
}

} // namespace

XMFLOAT3 ResolveCapsuleAgainstAabbs(const XMFLOAT3 &desiredPosition,
                                    const Capsule &capsule,
                                    const std::vector<Aabb> &aabbs,
                                    float worldHalfExtentMeters) {
  XMFLOAT3 resolved = desiredPosition;
  const float radius = std::max(0.01f, capsule.radius);
  const float bounds = std::max(0.0f, worldHalfExtentMeters - radius);

  resolved.x = Clamp(resolved.x, -bounds, bounds);
  resolved.z = Clamp(resolved.z, -bounds, bounds);

  for (int pass = 0; pass < 3; ++pass) {
    bool moved = false;
    for (const Aabb &aabb : aabbs) {
      moved |= ResolveCircleAgainstAabb(resolved.x, resolved.z, radius, aabb);
      resolved.x = Clamp(resolved.x, -bounds, bounds);
      resolved.z = Clamp(resolved.z, -bounds, bounds);
    }
    if (!moved)
      break;
  }

  resolved.y = 0.0f;
  return resolved;
}

XMFLOAT3 ResolveCapsuleAgainstColliders(
    const XMFLOAT3 &desiredPosition, const Capsule &capsule,
    const std::vector<Collider> &colliders, float worldHalfExtentMeters) {
  return ResolveCapsuleAgainstCollidersAndMesh(desiredPosition, capsule,
                                               colliders, {},
                                               worldHalfExtentMeters);
}

XMFLOAT3 ResolveCapsuleAgainstCollidersAndMesh(
    const XMFLOAT3 &desiredPosition, const Capsule &capsule,
    const std::vector<Collider> &colliders,
    const std::vector<MeshTriangle> &meshTriangles,
    float worldHalfExtentMeters) {
  XMFLOAT3 resolved = desiredPosition;
  const float radius = std::max(0.01f, capsule.radius);
  const float bounds = std::max(0.0f, worldHalfExtentMeters - radius);

  resolved.x = Clamp(resolved.x, -bounds, bounds);
  resolved.z = Clamp(resolved.z, -bounds, bounds);

  for (int pass = 0; pass < 4; ++pass) {
    bool moved = false;
    for (const Collider &collider : colliders) {
      moved |= ResolveCircleAgainstCollider(resolved.x, resolved.z, radius,
                                            collider);
      resolved.x = Clamp(resolved.x, -bounds, bounds);
      resolved.z = Clamp(resolved.z, -bounds, bounds);
    }
    for (const MeshTriangle &triangle : meshTriangles) {
      moved |= ResolveCircleAgainstMeshTriangle(resolved.x, resolved.z, radius,
                                                triangle);
      resolved.x = Clamp(resolved.x, -bounds, bounds);
      resolved.z = Clamp(resolved.z, -bounds, bounds);
    }
    if (!moved)
      break;
  }

  resolved.y = 0.0f;
  return resolved;
}

} // namespace CollisionSystem
