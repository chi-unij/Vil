#pragma once

#include "FeatureRegistry.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

enum class LabMeshKind : uint8_t {
  Cube,
  Plane,
  Sphere,
  Cylinder,
  Disc,
  Ring,
  Grid,
  Quad,
};

enum LabEntityFlags : uint32_t {
  LabEntityNone = 0,
  LabEntityTransparent = 1u << 0,
  LabEntityEmissive = 1u << 1,
  LabEntityWater = 1u << 2,
  LabEntityInstanced = 1u << 3,
  LabEntityColliderDebug = 1u << 4,
  LabEntityHologram = 1u << 5,
  LabEntityTelegraph = 1u << 6,
  LabEntityParticle = 1u << 7,
  LabEntityNoShadow = 1u << 8,
};

struct LabTransform {
  DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 rotation{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};

  [[nodiscard]] DirectX::XMMATRIX Matrix() const;
};

struct LabMaterial {
  DirectX::XMFLOAT4 baseColor{0.7f, 0.7f, 0.7f, 1.0f};
  float metallic = 0.0f;
  float roughness = 0.6f;
  float emissive = 0.0f;
  float pattern = 0.0f;
};

struct LabEntity {
  std::string name;
  std::string featureId;
  LabMeshKind mesh = LabMeshKind::Cube;
  LabTransform transform;
  LabMaterial material;
  uint32_t flags = LabEntityNone;
  uint32_t instanceCount = 1;
  bool active = true;
};

struct LabPointLight {
  DirectX::XMFLOAT3 position{0.0f, 2.0f, 0.0f};
  float range = 8.0f;
  DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
  float intensity = 4.0f;
};

enum class LabAttackType : uint32_t { Meteor = 0, Laser = 1, Sanctuary = 2 };

struct LabWorldFrame {
  DirectX::XMFLOAT3 sunDirection{0.3f, -1.0f, 0.2f};
  float sunIntensity = 3.0f;
  DirectX::XMFLOAT3 sunColor{1.0f, 0.96f, 0.86f};
  float timeSeconds = 0.0f;
  std::vector<LabPointLight> pointLights;
  LabAttackType attack = LabAttackType::Meteor;
  float attackProgress = 0.0f;
  bool attackResolving = false;
  bool phaseTwo = false;
  int bossHp = 5;
};

class FeatureWorld {
public:
  void Initialize();
  void Update(float deltaSeconds, const FeatureRegistry &features);

  [[nodiscard]] const std::vector<LabEntity> &Entities() const noexcept {
    return m_entities;
  }
  [[nodiscard]] const LabWorldFrame &Frame() const noexcept { return m_frame; }

  [[nodiscard]] DirectX::XMFLOAT3 CollisionSpherePosition() const noexcept {
    return m_collisionSpherePosition;
  }
  [[nodiscard]] DirectX::XMFLOAT3 CollisionSphereVelocity() const noexcept {
    return m_collisionSphereVelocity;
  }
  [[nodiscard]] uint64_t CollisionCount() const noexcept {
    return m_collisionCount;
  }

  void ResetSimulation();
  void ForceNextAttack(const FeatureRegistry &features);

private:
  void BuildWorld();
  void UpdateEntityAnimation(float deltaSeconds,
                             const FeatureRegistry &features);
  void UpdateCollision(float deltaSeconds, const FeatureRegistry &features);
  void UpdateBoss(float deltaSeconds, const FeatureRegistry &features);
  void UpdateLighting(const FeatureRegistry &features);
  LabAttackType NextAvailableAttack(const FeatureRegistry &features) const;

  std::vector<LabEntity> m_entities;
  LabWorldFrame m_frame;
  DirectX::XMFLOAT3 m_collisionSpherePosition{-8.0f, 1.0f, 4.0f};
  DirectX::XMFLOAT3 m_collisionSphereVelocity{4.2f, 0.0f, 2.8f};
  uint64_t m_collisionCount = 0;
  float m_attackClock = 0.0f;
  float m_phaseClock = 0.0f;
  size_t m_attackCursor = 0;
};
