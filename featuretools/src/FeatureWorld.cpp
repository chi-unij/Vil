#include "FeatureWorld.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace DirectX;

namespace {

LabEntity MakeEntity(std::string name, std::string featureId, LabMeshKind mesh,
                     XMFLOAT3 position, XMFLOAT3 scale, XMFLOAT4 color,
                     float metallic = 0.0f, float roughness = 0.6f,
                     float emissive = 0.0f, uint32_t flags = LabEntityNone) {
  LabEntity entity;
  entity.name = std::move(name);
  entity.featureId = std::move(featureId);
  entity.mesh = mesh;
  entity.transform.position = position;
  entity.transform.scale = scale;
  entity.material.baseColor = color;
  entity.material.metallic = metallic;
  entity.material.roughness = roughness;
  entity.material.emissive = emissive;
  entity.flags = flags;
  return entity;
}

float Clamp(float value, float low, float high) {
  return std::max(low, std::min(value, high));
}

} // namespace

XMMATRIX LabTransform::Matrix() const {
  return XMMatrixScaling(scale.x, scale.y, scale.z) *
         XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
         XMMatrixTranslation(position.x, position.y, position.z);
}

void FeatureWorld::Initialize() {
  BuildWorld();
  ResetSimulation();
}

void FeatureWorld::BuildWorld() {
  m_entities.clear();
  m_entities.push_back(MakeEntity(
      "Ground", "ground", LabMeshKind::Plane, {0.0f, 0.0f, 0.0f},
      {32.0f, 1.0f, 32.0f}, {0.12f, 0.22f, 0.20f, 1.0f}, 0.05f, 0.78f));
  m_entities.back().material.pattern = 1.0f;

  m_entities.push_back(MakeEntity(
      "WorldGrid", "grid", LabMeshKind::Grid, {0.0f, 0.018f, 0.0f},
      {32.0f, 1.0f, 32.0f}, {0.22f, 0.72f, 0.78f, 0.35f}, 0.0f, 1.0f,
      0.4f, LabEntityTransparent | LabEntityEmissive | LabEntityNoShadow));

  for (int side = -1; side <= 1; side += 2) {
    for (int row = 0; row < 6; ++row) {
      auto pillar = MakeEntity(
          "EnvironmentPillar", "environment", LabMeshKind::Cylinder,
          {side * 9.0f, 2.0f, -10.0f + row * 4.0f}, {0.9f, 4.0f, 0.9f},
          {0.30f, 0.24f, 0.22f, 1.0f}, 0.15f, 0.72f);
      pillar.material.pattern = 2.0f;
      m_entities.push_back(std::move(pillar));

      auto lantern = MakeEntity(
          "Lantern", "environment", LabMeshKind::Sphere,
          {side * 9.0f, 4.45f, -10.0f + row * 4.0f}, {0.34f, 0.34f, 0.34f},
          {1.0f, 0.34f, 0.08f, 1.0f}, 0.1f, 0.3f, 5.0f,
          LabEntityEmissive | LabEntityNoShadow);
      m_entities.push_back(std::move(lantern));
    }
  }

  auto shrine = MakeEntity(
      "Shrine", "environment", LabMeshKind::Cube, {0.0f, 3.0f, 15.0f},
      {7.0f, 5.5f, 2.2f}, {0.38f, 0.08f, 0.06f, 1.0f}, 0.15f, 0.56f);
  shrine.material.pattern = 3.0f;
  m_entities.push_back(std::move(shrine));
  m_entities.push_back(MakeEntity(
      "ShrineRoof", "environment", LabMeshKind::Cube, {0.0f, 6.15f, 15.0f},
      {8.5f, 0.55f, 3.0f}, {0.08f, 0.08f, 0.10f, 1.0f}, 0.55f, 0.35f));

  for (int index = 0; index < 6; ++index) {
    const float metallic = index / 5.0f;
    auto sphere = MakeEntity(
        "MaterialSphere", "material_gallery", LabMeshKind::Sphere,
        {-6.25f + index * 2.5f, 1.25f, -7.0f}, {1.05f, 1.05f, 1.05f},
        {0.18f + 0.10f * index, 0.48f, 0.72f - 0.07f * index, 1.0f},
        metallic, 0.12f + index * 0.15f);
    sphere.material.pattern = static_cast<float>(index % 4);
    m_entities.push_back(std::move(sphere));
  }

  auto instances = MakeEntity(
      "InstancedForest", "gpu_instancing", LabMeshKind::Cube,
      {0.0f, 0.8f, 0.0f}, {0.35f, 1.6f, 0.35f},
      {0.10f, 0.38f, 0.20f, 1.0f}, 0.0f, 0.9f, 0.0f,
      LabEntityInstanced);
  instances.instanceCount = 49;
  m_entities.push_back(std::move(instances));

  m_entities.push_back(MakeEntity(
      "Water", "water", LabMeshKind::Plane, {0.0f, 0.22f, 1.0f},
      {7.0f, 1.0f, 22.0f}, {0.04f, 0.38f, 0.48f, 0.56f}, 0.05f, 0.12f,
      0.15f, LabEntityTransparent | LabEntityWater | LabEntityNoShadow));

  m_entities.push_back(MakeEntity(
      "CollisionBoxA", "collision", LabMeshKind::Cube,
      {-4.5f, 1.0f, 4.0f}, {1.3f, 2.0f, 2.5f},
      {0.35f, 0.16f, 0.14f, 1.0f}, 0.1f, 0.7f));
  m_entities.push_back(MakeEntity(
      "CollisionBoxB", "collision", LabMeshKind::Cube,
      {2.0f, 1.0f, 7.0f}, {2.0f, 2.0f, 1.2f},
      {0.35f, 0.16f, 0.14f, 1.0f}, 0.1f, 0.7f));
  m_entities.push_back(MakeEntity(
      "CollisionMover", "collision", LabMeshKind::Sphere,
      m_collisionSpherePosition, {0.75f, 0.75f, 0.75f},
      {0.18f, 0.92f, 0.78f, 1.0f}, 0.15f, 0.28f, 1.2f,
      LabEntityEmissive));
  m_entities.push_back(MakeEntity(
      "CollisionDebugA", "collision_debug", LabMeshKind::Cube,
      {-4.5f, 1.0f, 4.0f}, {1.38f, 2.08f, 2.58f},
      {1.0f, 0.25f, 0.2f, 0.72f}, 0.0f, 1.0f, 1.0f,
      LabEntityTransparent | LabEntityColliderDebug | LabEntityEmissive |
          LabEntityNoShadow));
  m_entities.push_back(MakeEntity(
      "CollisionDebugB", "collision_debug", LabMeshKind::Cube,
      {2.0f, 1.0f, 7.0f}, {2.08f, 2.08f, 1.28f},
      {1.0f, 0.25f, 0.2f, 0.72f}, 0.0f, 1.0f, 1.0f,
      LabEntityTransparent | LabEntityColliderDebug | LabEntityEmissive |
          LabEntityNoShadow));

  m_entities.push_back(MakeEntity(
      "Boss", "boss", LabMeshKind::Sphere, {0.0f, 4.8f, 10.5f},
      {2.3f, 2.3f, 2.3f}, {0.42f, 0.04f, 0.10f, 1.0f}, 0.45f, 0.23f,
      2.2f, LabEntityEmissive));
  m_entities.push_back(MakeEntity(
      "MeteorTelegraph", "meteor", LabMeshKind::Disc, {-3.5f, 0.06f, 1.5f},
      {3.2f, 1.0f, 3.2f}, {1.0f, 0.08f, 0.04f, 0.54f}, 0.0f, 1.0f, 4.0f,
      LabEntityTransparent | LabEntityTelegraph | LabEntityEmissive |
          LabEntityNoShadow));
  m_entities.push_back(MakeEntity(
      "LaserTelegraph", "laser", LabMeshKind::Cube, {0.0f, 0.08f, 1.5f},
      {1.2f, 0.08f, 19.0f}, {1.0f, 0.05f, 0.12f, 0.52f}, 0.0f, 1.0f, 5.0f,
      LabEntityTransparent | LabEntityTelegraph | LabEntityEmissive |
          LabEntityNoShadow));
  m_entities.push_back(MakeEntity(
      "SanctuarySeal", "sanctuary", LabMeshKind::Ring, {3.5f, 0.07f, 2.5f},
      {4.0f, 1.0f, 4.0f}, {0.15f, 0.95f, 0.88f, 0.62f}, 0.0f, 1.0f, 4.0f,
      LabEntityTransparent | LabEntityTelegraph | LabEntityEmissive |
          LabEntityNoShadow));

  for (int charge = 0; charge < 3; ++charge) {
    m_entities.push_back(MakeEntity(
        "MirrorCharge", "mirror_charges", LabMeshKind::Sphere,
        {-2.8f + charge * 2.8f, 1.25f, 3.0f}, {0.38f, 0.38f, 0.38f},
        {0.18f, 0.82f, 1.0f, 1.0f}, 0.15f, 0.12f, 7.0f,
        LabEntityEmissive | LabEntityNoShadow));
  }

  m_entities.push_back(MakeEntity(
      "PhonePanel", "phone_hologram", LabMeshKind::Cube,
      {-7.0f, 3.3f, -2.0f}, {2.2f, 3.4f, 0.12f},
      {0.04f, 0.28f, 0.36f, 0.58f}, 0.2f, 0.2f, 3.0f,
      LabEntityTransparent | LabEntityHologram | LabEntityEmissive |
          LabEntityNoShadow));
  for (int node = 0; node < 4; ++node) {
    m_entities.push_back(MakeEntity(
        "PhoneNode", "phone_hologram", LabMeshKind::Sphere,
        {-7.6f + node * 0.42f, 2.6f + std::sin(node * 1.4f) * 0.8f, -1.78f},
        {0.13f, 0.13f, 0.13f}, {0.35f, 1.0f, 0.92f, 1.0f}, 0.0f, 0.2f,
        8.0f, LabEntityEmissive | LabEntityNoShadow));
  }

  auto sparks = MakeEntity(
      "SparkParticles", "particles", LabMeshKind::Quad, {0.0f, 2.0f, 0.0f},
      {0.12f, 0.12f, 0.12f}, {1.0f, 0.44f, 0.12f, 0.62f}, 0.0f, 1.0f,
      5.0f, LabEntityTransparent | LabEntityParticle | LabEntityEmissive |
                LabEntityNoShadow | LabEntityInstanced);
  sparks.instanceCount = 96;
  m_entities.push_back(std::move(sparks));

  auto counterBurst = MakeEntity(
      "CounterBurst", "counter_vfx", LabMeshKind::Quad,
      {-4.0f, 1.4f, 4.0f}, {0.09f, 0.09f, 0.09f},
      {0.24f, 0.92f, 1.0f, 0.72f}, 0.0f, 1.0f, 7.0f,
      LabEntityTransparent | LabEntityParticle | LabEntityEmissive |
          LabEntityNoShadow | LabEntityInstanced);
  counterBurst.instanceCount = 36;
  m_entities.push_back(std::move(counterBurst));

  auto rain = MakeEntity(
      "RainParticles", "rain", LabMeshKind::Quad, {0.0f, 0.0f, 0.0f},
      {0.035f, 0.55f, 0.035f}, {0.48f, 0.78f, 1.0f, 0.42f}, 0.0f, 1.0f,
      2.0f, LabEntityTransparent | LabEntityParticle | LabEntityEmissive |
                LabEntityNoShadow | LabEntityInstanced);
  rain.instanceCount = 420;
  m_entities.push_back(std::move(rain));
}

void FeatureWorld::ResetSimulation() {
  m_frame = {};
  m_collisionSpherePosition = {-8.0f, 1.0f, 4.0f};
  m_collisionSphereVelocity = {4.2f, 0.0f, 2.8f};
  m_collisionCount = 0;
  m_attackClock = 0.0f;
  m_phaseClock = 0.0f;
  m_attackCursor = 0;
  for (auto &entity : m_entities) {
    if (entity.name == "CollisionMover")
      entity.transform.position = m_collisionSpherePosition;
  }
}

void FeatureWorld::Update(float deltaSeconds,
                          const FeatureRegistry &features) {
  const float delta = Clamp(deltaSeconds, 0.0f, 0.1f);
  m_frame.timeSeconds += delta;
  UpdateLighting(features);
  UpdateEntityAnimation(delta, features);
  UpdateCollision(delta, features);
  UpdateBoss(delta, features);

  m_frame.pointLights.clear();
  if (features.Enabled("point_lights")) {
    for (int light = 0; light < 6; ++light) {
      const float angle = m_frame.timeSeconds * 0.32f + light * 1.0472f;
      LabPointLight point;
      point.position = {std::cos(angle) * 7.5f, 2.8f + (light % 2) * 1.6f,
                        std::sin(angle) * 7.5f + 2.0f};
      point.range = 9.0f;
      point.color = light % 3 == 0
                        ? XMFLOAT3{0.10f, 0.75f, 1.0f}
                        : light % 3 == 1 ? XMFLOAT3{1.0f, 0.18f, 0.08f}
                                         : XMFLOAT3{0.45f, 1.0f, 0.42f};
      point.intensity = 6.5f;
      m_frame.pointLights.push_back(point);
    }
  }
}

void FeatureWorld::UpdateLighting(const FeatureRegistry &features) {
  if (!features.Enabled("time_of_day")) {
    m_frame.sunDirection = {0.32f, -0.92f, 0.22f};
    m_frame.sunColor = {1.0f, 0.94f, 0.84f};
    m_frame.sunIntensity = features.Enabled("directional_light") ? 3.2f : 0.0f;
    return;
  }
  const float dayAngle = m_frame.timeSeconds * 0.055f;
  const float height = 0.28f + std::abs(std::sin(dayAngle)) * 0.72f;
  m_frame.sunDirection = {std::cos(dayAngle) * 0.55f, -height,
                          std::sin(dayAngle) * 0.55f};
  const float sunset = 1.0f - height;
  m_frame.sunColor = {1.0f, 0.94f - sunset * 0.25f,
                      0.82f - sunset * 0.42f};
  m_frame.sunIntensity =
      features.Enabled("directional_light") ? 2.4f + height * 2.1f : 0.0f;
}

void FeatureWorld::UpdateEntityAnimation(float deltaSeconds,
                                         const FeatureRegistry &features) {
  if (!features.Enabled("animation"))
    return;
  for (size_t index = 0; index < m_entities.size(); ++index) {
    auto &entity = m_entities[index];
    if (entity.name == "MaterialSphere") {
      entity.transform.rotation.y += deltaSeconds * (0.35f + index * 0.01f);
      entity.transform.position.y =
          1.25f + std::sin(m_frame.timeSeconds * 1.3f + index) * 0.12f;
    } else if (entity.name == "MirrorCharge") {
      entity.transform.position.y =
          1.25f + std::sin(m_frame.timeSeconds * 2.2f + index) * 0.35f;
      entity.transform.rotation.y += deltaSeconds * 2.0f;
    } else if (entity.name == "Boss") {
      entity.transform.position.y =
          4.8f + std::sin(m_frame.timeSeconds * 1.1f) * 0.35f;
      entity.transform.rotation.y += deltaSeconds * 0.28f;
    }
  }
}

void FeatureWorld::UpdateCollision(float deltaSeconds,
                                   const FeatureRegistry &features) {
  if (!features.Enabled("collision"))
    return;

  XMFLOAT3 next{
      m_collisionSpherePosition.x + m_collisionSphereVelocity.x * deltaSeconds,
      1.0f,
      m_collisionSpherePosition.z + m_collisionSphereVelocity.z * deltaSeconds};
  constexpr float radius = 0.75f;

  if (next.x < -11.0f + radius || next.x > 11.0f - radius) {
    m_collisionSphereVelocity.x *= -1.0f;
    next.x = Clamp(next.x, -11.0f + radius, 11.0f - radius);
    ++m_collisionCount;
  }
  if (next.z < -1.0f + radius || next.z > 11.0f - radius) {
    m_collisionSphereVelocity.z *= -1.0f;
    next.z = Clamp(next.z, -1.0f + radius, 11.0f - radius);
    ++m_collisionCount;
  }

  struct Box2D {
    float x;
    float z;
    float halfX;
    float halfZ;
  };
  constexpr std::array<Box2D, 2> boxes{{{-4.5f, 4.0f, 1.3f, 2.5f},
                                         {2.0f, 7.0f, 2.0f, 1.2f}}};
  for (const auto &box : boxes) {
    const float closestX = Clamp(next.x, box.x - box.halfX, box.x + box.halfX);
    const float closestZ = Clamp(next.z, box.z - box.halfZ, box.z + box.halfZ);
    const float dx = next.x - closestX;
    const float dz = next.z - closestZ;
    const float distanceSquared = dx * dx + dz * dz;
    if (distanceSquared >= radius * radius)
      continue;
    if (std::abs(dx) > std::abs(dz))
      m_collisionSphereVelocity.x *= -1.0f;
    else
      m_collisionSphereVelocity.z *= -1.0f;
    next = m_collisionSpherePosition;
    ++m_collisionCount;
  }

  m_collisionSpherePosition = next;
  for (auto &entity : m_entities) {
    if (entity.name == "CollisionMover") {
      entity.transform.position = next;
      break;
    }
  }
}

LabAttackType FeatureWorld::NextAvailableAttack(
    const FeatureRegistry &features) const {
  const std::array<std::pair<std::string_view, LabAttackType>, 3> attacks{{
      {"meteor", LabAttackType::Meteor},
      {"laser", LabAttackType::Laser},
      {"sanctuary", LabAttackType::Sanctuary},
  }};
  for (size_t offset = 1; offset <= attacks.size(); ++offset) {
    const size_t candidate = (m_attackCursor + offset) % attacks.size();
    if (features.Enabled(attacks[candidate].first))
      return attacks[candidate].second;
  }
  return m_frame.attack;
}

void FeatureWorld::ForceNextAttack(const FeatureRegistry &features) {
  const LabAttackType next = NextAvailableAttack(features);
  m_frame.attack = next;
  m_attackCursor = static_cast<size_t>(next);
  m_attackClock = 0.0f;
}

void FeatureWorld::UpdateBoss(float deltaSeconds,
                              const FeatureRegistry &features) {
  if (!features.Enabled("boss")) {
    m_frame.attackProgress = 0.0f;
    m_frame.attackResolving = false;
    return;
  }

  m_phaseClock += deltaSeconds;
  m_frame.phaseTwo = features.Enabled("phase2") && m_phaseClock >= 18.0f;
  m_frame.bossHp = m_frame.phaseTwo ? 2 : 5;
  m_attackClock += deltaSeconds;
  constexpr float attackDuration = 6.0f;
  m_frame.attackProgress = std::fmod(m_attackClock, attackDuration) / attackDuration;
  m_frame.attackResolving = m_frame.attackProgress > 0.64f &&
                            m_frame.attackProgress < 0.79f;

  if (features.Enabled("auto_demo") && m_attackClock >= attackDuration) {
    ForceNextAttack(features);
  }

  for (auto &entity : m_entities) {
    if (entity.name == "MeteorTelegraph") {
      entity.active = m_frame.attack == LabAttackType::Meteor;
      const float pulse = 0.82f + std::sin(m_frame.attackProgress * 24.0f) * 0.12f;
      entity.transform.scale = {3.2f * pulse, 1.0f, 3.2f * pulse};
      entity.material.baseColor.w = m_frame.attackResolving ? 0.88f : 0.46f;
    } else if (entity.name == "LaserTelegraph") {
      entity.active = m_frame.attack == LabAttackType::Laser;
      entity.transform.scale.x =
          0.7f + m_frame.attackProgress * (m_frame.phaseTwo ? 1.8f : 1.2f);
      entity.material.baseColor.w = m_frame.attackResolving ? 0.9f : 0.48f;
    } else if (entity.name == "SanctuarySeal") {
      entity.active = m_frame.attack == LabAttackType::Sanctuary;
      const float pulse = 0.92f + std::sin(m_frame.attackProgress * 18.0f) * 0.08f;
      entity.transform.scale = {4.0f * pulse, 1.0f, 4.0f * pulse};
      entity.material.baseColor.w = m_frame.attackResolving ? 0.92f : 0.58f;
    } else if (entity.name == "Boss") {
      entity.material.baseColor = m_frame.phaseTwo
                                      ? XMFLOAT4{0.62f, 0.02f, 0.06f, 1.0f}
                                      : XMFLOAT4{0.42f, 0.04f, 0.10f, 1.0f};
      entity.material.emissive = m_frame.phaseTwo ? 5.0f : 2.2f;
    }
  }
}
