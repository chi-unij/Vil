#pragma once

#include "particle.h"

#include <DirectXMath.h>
#include <random>

class RainParticle : public Particle {
public:
  RainParticle(const DirectX::XMVECTOR &position,
               const DirectX::XMVECTOR &velocity, double lifeTime,
               float width, float length, float alpha, float slant);

  void Update(double elapsedTime) override;
  ParticleVisual GetVisual() const override;

private:
  float m_width = 0.02f;
  float m_length = 1.0f;
  float m_alpha = 0.45f;
  float m_slant = -0.24f;
};

class RainEmitter : public Emitter {
public:
  RainEmitter(size_t capacity, float areaHalfExtent, float topY, float bottomY,
              double particlesPerSecond, bool isEmit);

  void WarmStart();

protected:
  Particle *createParticle() override;

private:
  float RandomRange(float minValue, float maxValue);

  std::mt19937 m_random{std::random_device{}()};
  float m_areaHalfExtent = 32.0f;
  float m_topY = 12.0f;
  float m_bottomY = 0.18f;
};