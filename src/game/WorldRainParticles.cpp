#include "game/WorldRainParticles.h"

#include <algorithm>

using namespace DirectX;

RainParticle::RainParticle(const XMVECTOR &position, const XMVECTOR &velocity,
                           double lifeTime, float width, float length,
                           float alpha, float slant)
    : Particle(position, velocity, lifeTime), m_width(width),
      m_length(length), m_alpha(alpha), m_slant(slant) {}

void RainParticle::Update(double elapsedTime) {
  AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsedTime)));
  Particle::Update(elapsedTime);
}

ParticleVisual RainParticle::GetVisual() const {
  const float ratio = static_cast<float>(
      std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));

  ParticleVisual visual{};
  XMStoreFloat3(&visual.position, GetPosition());
  visual.size = {m_width, m_length};
  visual.color = {0.58f, 0.82f, 1.0f, m_alpha * (1.0f - ratio * 0.35f)};
  visual.shape = 1.0f;
  visual.slant = m_slant;
  return visual;
}

RainEmitter::RainEmitter(size_t capacity, float areaHalfExtent, float topY,
                         float bottomY, double particlesPerSecond,
                         bool isEmit)
    : Emitter(capacity, XMVectorZero(), particlesPerSecond, isEmit),
      m_areaHalfExtent(areaHalfExtent), m_topY(topY), m_bottomY(bottomY) {}

void RainEmitter::WarmStart() {
  while (!IsFull()) {
    if (!SpawnParticle())
      break;
  }
}

Particle *RainEmitter::createParticle() {
  const float x = RandomRange(-m_areaHalfExtent, m_areaHalfExtent);
  const float z = RandomRange(-m_areaHalfExtent, m_areaHalfExtent);
  const float y = RandomRange(m_bottomY, m_topY);
  const float fallSpeed = RandomRange(10.5f, 15.0f);
  const float windX = RandomRange(-1.9f, -1.1f);
  const float windZ = RandomRange(0.20f, 0.75f);
  const float life = std::max((y - m_bottomY) / fallSpeed, 0.12f);
  const float width = RandomRange(0.010f, 0.022f);
  const float length = RandomRange(0.62f, 1.24f);
  const float alpha = RandomRange(0.28f, 0.48f);
  const float slant = RandomRange(-0.34f, -0.20f);

  return new RainParticle(XMVectorSet(x, y, z, 0.0f),
                          XMVectorSet(windX, -fallSpeed, windZ, 0.0f), life,
                          width, length, alpha, slant);
}

float RainEmitter::RandomRange(float minValue, float maxValue) {
  std::uniform_real_distribution<float> dist(minValue, maxValue);
  return dist(m_random);
}