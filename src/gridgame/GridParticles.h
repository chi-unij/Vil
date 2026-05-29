// ======================================
// File: GridParticles.h
// Purpose: Game-specific particle emitters for Grid Gauntlet neon VFX.
//          Fire embers (orange rising), ice crystals (cyan drifting).
// ======================================

#pragma once

#include "../particle.h"

#include <DirectXMath.h>
#include <random>

// ---- Fire Ember: small orange particle rising from fire tiles ----

class FireEmberParticle : public Particle {
  float m_scale{0.12f};
  float m_alpha{1.0f};

public:
  FireEmberParticle(const DirectX::XMVECTOR &pos,
                    const DirectX::XMVECTOR &vel, double life)
      : Particle(pos, vel, life) {}

  void Update(double dt) override {
    float ratio =
        static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    m_scale = 0.12f * (1.0f - ratio);
    m_alpha = 1.0f - ratio * ratio; // slow fade then fast drop
    AddPosition(
        DirectX::XMVectorScale(GetVelocity(), static_cast<float>(dt)));
    // Slight upward drift (buoyancy).
    AddVelocity(DirectX::XMVectorScale(
        DirectX::XMVectorSet(0.0f, 0.5f, 0.0f, 0.0f),
        static_cast<float>(dt)));
    Particle::Update(dt);
  }

  ParticleVisual GetVisual() const override {
    ParticleVisual v{};
    DirectX::XMStoreFloat3(&v.position, GetPosition());
    v.scale = m_scale;
    // Orange -> red shift.
    float ratio =
        static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    v.color = {1.0f, 0.5f - ratio * 0.3f, 0.1f - ratio * 0.08f, m_alpha};
    return v;
  }
};

class FireEmberEmitter : public Emitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  FireEmberEmitter(size_t cap, const DirectX::XMVECTOR &pos, double rate,
                   bool emit)
      : Emitter(cap, pos, rate, emit) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.4f, 0.4f);
    std::uniform_real_distribution<float> spd(0.3f, 0.8f);
    std::uniform_real_distribution<float> life(0.6f, 1.5f);

    auto spawn = DirectX::XMVectorAdd(
        GetPosition(),
        DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    float s = spd(m_mt);
    auto vel = DirectX::XMVectorSet(off(m_mt) * 0.2f, s, off(m_mt) * 0.2f, 0.0f);
    return new FireEmberParticle(spawn, vel, life(m_mt));
  }
};

// ---- Ice Crystal: small cyan particle drifting up from ice tiles ----

class IceCrystalParticle : public Particle {
  float m_scale{0.08f};
  float m_alpha{1.0f};

public:
  IceCrystalParticle(const DirectX::XMVECTOR &pos,
                     const DirectX::XMVECTOR &vel, double life)
      : Particle(pos, vel, life) {}

  void Update(double dt) override {
    float ratio =
        static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    m_scale = 0.08f * (1.0f - ratio * 0.5f); // slight shrink
    m_alpha = 1.0f - ratio;
    AddPosition(
        DirectX::XMVectorScale(GetVelocity(), static_cast<float>(dt)));
    Particle::Update(dt);
  }

  ParticleVisual GetVisual() const override {
    ParticleVisual v{};
    DirectX::XMStoreFloat3(&v.position, GetPosition());
    v.scale = m_scale;
    v.color = {0.2f, 0.7f, 1.0f, m_alpha * 0.8f}; // bright cyan
    return v;
  }
};

class IceCrystalEmitter : public Emitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  IceCrystalEmitter(size_t cap, const DirectX::XMVECTOR &pos, double rate,
                    bool emit)
      : Emitter(cap, pos, rate, emit) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(0.1f, 0.4f);
    std::uniform_real_distribution<float> drift(-0.15f, 0.15f);
    std::uniform_real_distribution<float> life(1.0f, 2.5f);

    auto spawn = DirectX::XMVectorAdd(
        GetPosition(),
        DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel =
        DirectX::XMVectorSet(drift(m_mt), spd(m_mt), drift(m_mt), 0.0f);
    return new IceCrystalParticle(spawn, vel, life(m_mt));
  }
};

// ======================================================================
// Phase 8B: Combat VFX
// ======================================================================

// ---- Generic burst particle with gravity support ----

class BurstParticle : public Particle {
  float m_initScale{};
  DirectX::XMFLOAT4 m_colorStart{};
  DirectX::XMFLOAT4 m_colorEnd{};
  float m_gravity{};

public:
  BurstParticle(const DirectX::XMVECTOR &pos, const DirectX::XMVECTOR &vel,
                double life, float scale, const DirectX::XMFLOAT4 &colorStart,
                const DirectX::XMFLOAT4 &colorEnd, float gravity = 0.0f)
      : Particle(pos, vel, life), m_initScale(scale),
        m_colorStart(colorStart), m_colorEnd(colorEnd), m_gravity(gravity) {}

  void Update(double dt) override {
    float fdt = static_cast<float>(dt);
    AddPosition(DirectX::XMVectorScale(GetVelocity(), fdt));
    if (m_gravity != 0.0f)
      AddVelocity(DirectX::XMVectorSet(0.0f, -m_gravity * fdt, 0.0f, 0.0f));
    Particle::Update(dt);
  }

  ParticleVisual GetVisual() const override {
    float ratio = static_cast<float>(
        std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    float alpha = 1.0f - ratio * ratio;
    ParticleVisual v{};
    DirectX::XMStoreFloat3(&v.position, GetPosition());
    v.scale = m_initScale * (1.0f - ratio * 0.5f);
    v.color = {m_colorStart.x + (m_colorEnd.x - m_colorStart.x) * ratio,
               m_colorStart.y + (m_colorEnd.y - m_colorStart.y) * ratio,
               m_colorStart.z + (m_colorEnd.z - m_colorStart.z) * ratio,
               alpha};
    return v;
  }
};

// ---- #5 TowerFireBurst: orange burst when tower fires ----

class TowerFireBurstEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  TowerFireBurstEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(24, pos, 22) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(1.0f, 3.0f);
    std::uniform_real_distribution<float> life(0.3f, 0.6f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 2.0f, s, off(m_mt) * 2.0f, 0.0f);
    return new BurstParticle(spawn, vel, life(m_mt), 0.10f,
                             {1.0f, 0.4f, 0.1f, 1.0f},
                             {0.4f, 0.05f, 0.02f, 1.0f});
  }
};

// ---- #6 BeamImpactSparks: white-yellow sparks per attack tile ----

class BeamImpactSparksEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  BeamImpactSparksEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(12, pos, 10) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(1.5f, 4.0f);
    std::uniform_real_distribution<float> life(0.2f, 0.4f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 3.0f, s, off(m_mt) * 3.0f, 0.0f);
    return new BurstParticle(spawn, vel, life(m_mt), 0.06f,
                             {1.0f, 0.9f, 0.5f, 1.0f},
                             {1.0f, 0.6f, 0.2f, 1.0f});
  }
};

// ---- #7 DamageHitBurst: red + white sparks on player damage ----

class DamageHitBurstEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  DamageHitBurstEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(20, pos, 18) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(1.0f, 3.5f);
    std::uniform_real_distribution<float> life(0.3f, 0.5f);
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 2.5f, s, off(m_mt) * 2.5f, 0.0f);
    bool isWhite = chance(m_mt) < 0.3f;
    auto cStart = isWhite ? DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f}
                          : DirectX::XMFLOAT4{1.0f, 0.15f, 0.1f, 1.0f};
    auto cEnd = isWhite ? DirectX::XMFLOAT4{0.8f, 0.8f, 0.8f, 1.0f}
                        : DirectX::XMFLOAT4{0.5f, 0.0f, 0.0f, 1.0f};
    return new BurstParticle(spawn, vel, life(m_mt), 0.08f, cStart, cEnd);
  }
};

// ---- #8 WallDebris: orange + white chunks with gravity arc ----

class WallDebrisEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  WallDebrisEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(30, pos, 27) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.4f, 0.4f);
    std::uniform_real_distribution<float> spd(2.0f, 5.0f);
    std::uniform_real_distribution<float> life(0.5f, 1.0f);
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 3.0f, s, off(m_mt) * 3.0f, 0.0f);
    bool isWhite = chance(m_mt) < 0.4f;
    auto cStart = isWhite ? DirectX::XMFLOAT4{1.0f, 0.9f, 0.8f, 1.0f}
                          : DirectX::XMFLOAT4{1.0f, 0.5f, 0.1f, 1.0f};
    auto cEnd = isWhite ? DirectX::XMFLOAT4{0.5f, 0.4f, 0.3f, 1.0f}
                        : DirectX::XMFLOAT4{0.3f, 0.1f, 0.0f, 1.0f};
    return new BurstParticle(spawn, vel, life(m_mt), 0.12f, cStart, cEnd, 8.0f);
  }
};

// ---- #9 CrumbleDebris: gray-brown chunks with heavy gravity ----

class CrumbleDebrisEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  CrumbleDebrisEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(20, pos, 17) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(1.0f, 3.0f);
    std::uniform_real_distribution<float> life(0.4f, 0.8f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 2.0f, s, off(m_mt) * 2.0f, 0.0f);
    return new BurstParticle(spawn, vel, life(m_mt), 0.10f,
                             {0.6f, 0.5f, 0.4f, 1.0f},
                             {0.3f, 0.25f, 0.2f, 1.0f}, 10.0f);
  }
};

// ======================================================================
// Phase 8C: Hazard & Environment VFX
// ======================================================================

// ---- #10 LightningStrikeSparks: electric blue burst ----

class LightningStrikeSparksEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  LightningStrikeSparksEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(16, pos, 14) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(2.0f, 5.0f);
    std::uniform_real_distribution<float> life(0.15f, 0.35f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 4.0f, s, off(m_mt) * 4.0f, 0.0f);
    // HDR blue > 1.0 for bloom glow.
    return new BurstParticle(spawn, vel, life(m_mt), 0.07f,
                             {0.3f, 0.6f, 2.0f, 1.0f},
                             {0.1f, 0.3f, 1.0f, 1.0f});
  }
};

// ---- #11 SpikeTrapSparks: dark red metallic ----

class SpikeTrapSparksEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  SpikeTrapSparksEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(12, pos, 10) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(1.5f, 3.5f);
    std::uniform_real_distribution<float> life(0.2f, 0.4f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 2.0f, s, off(m_mt) * 2.0f, 0.0f);
    return new BurstParticle(spawn, vel, life(m_mt), 0.06f,
                             {0.7f, 0.1f, 0.05f, 1.0f},
                             {0.3f, 0.05f, 0.02f, 1.0f});
  }
};

// ---- #12 GoalBeacon: continuous gold particles with spiral drift ----

class GoalBeaconParticle : public Particle {
  float m_scale{0.06f};
  float m_alpha{1.0f};
  float m_angle{};

public:
  GoalBeaconParticle(const DirectX::XMVECTOR &pos,
                     const DirectX::XMVECTOR &vel, double life, float angle)
      : Particle(pos, vel, life), m_angle(angle) {}

  void Update(double dt) override {
    float fdt = static_cast<float>(dt);
    float ratio = static_cast<float>(
        std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    m_scale = 0.06f * (1.0f - ratio * 0.5f);
    m_alpha = 1.0f - ratio;
    // Spiral drift.
    m_angle += fdt * 2.0f;
    float spiralX = cosf(m_angle) * 0.3f * fdt;
    float spiralZ = sinf(m_angle) * 0.3f * fdt;
    AddPosition(DirectX::XMVectorScale(GetVelocity(), fdt));
    AddPosition(DirectX::XMVectorSet(spiralX, 0.0f, spiralZ, 0.0f));
    Particle::Update(dt);
  }

  ParticleVisual GetVisual() const override {
    ParticleVisual v{};
    DirectX::XMStoreFloat3(&v.position, GetPosition());
    v.scale = m_scale;
    // HDR gold > 1.0 for bloom.
    v.color = {1.5f, 1.2f, 0.15f, m_alpha};
    return v;
  }
};

class GoalBeaconEmitter : public Emitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  GoalBeaconEmitter(size_t cap, const DirectX::XMVECTOR &pos, double rate,
                    bool emit)
      : Emitter(cap, pos, rate, emit) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.3f, 0.3f);
    std::uniform_real_distribution<float> spd(0.3f, 0.7f);
    std::uniform_real_distribution<float> life(1.5f, 3.0f);
    std::uniform_real_distribution<float> angle(0.0f, 6.283f);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 0.1f, spd(m_mt),
                                    off(m_mt) * 0.1f, 0.0f);
    return new GoalBeaconParticle(spawn, vel, life(m_mt), angle(m_mt));
  }
};

// ---- #13 TowerIdleWisps: dim red-orange orbital particles ----

class TowerIdleWispParticle : public Particle {
  float m_scale{0.05f};
  float m_alpha{1.0f};
  float m_angle{};

public:
  TowerIdleWispParticle(const DirectX::XMVECTOR &pos,
                        const DirectX::XMVECTOR &vel, double life, float angle)
      : Particle(pos, vel, life), m_angle(angle) {}

  void Update(double dt) override {
    float fdt = static_cast<float>(dt);
    float ratio = static_cast<float>(
        std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    m_scale = 0.05f * (1.0f - ratio * 0.3f);
    m_alpha = 0.7f * (1.0f - ratio);
    m_angle += fdt * 1.5f;
    float orbX = cosf(m_angle) * 0.5f * fdt;
    float orbZ = sinf(m_angle) * 0.5f * fdt;
    AddPosition(DirectX::XMVectorScale(GetVelocity(), fdt));
    AddPosition(DirectX::XMVectorSet(orbX, 0.0f, orbZ, 0.0f));
    Particle::Update(dt);
  }

  ParticleVisual GetVisual() const override {
    ParticleVisual v{};
    DirectX::XMStoreFloat3(&v.position, GetPosition());
    v.scale = m_scale;
    float ratio = static_cast<float>(
        std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
    v.color = {0.8f - ratio * 0.3f, 0.25f - ratio * 0.1f, 0.05f, m_alpha};
    return v;
  }
};

class TowerIdleWispEmitter : public Emitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  TowerIdleWispEmitter(size_t cap, const DirectX::XMVECTOR &pos, double rate,
                       bool emit)
      : Emitter(cap, pos, rate, emit) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.2f, 0.2f);
    std::uniform_real_distribution<float> spd(0.1f, 0.3f);
    std::uniform_real_distribution<float> life(1.0f, 2.0f);
    std::uniform_real_distribution<float> angle(0.0f, 6.283f);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 0.1f, spd(m_mt),
                                    off(m_mt) * 0.1f, 0.0f);
    return new TowerIdleWispParticle(spawn, vel, life(m_mt), angle(m_mt));
  }
};

// ======================================================================
// Phase 8D: Player VFX
// ======================================================================

// ---- #14 PlayerMoveSparks: cyan burst at old tile on movement ----

class PlayerMoveSparksEmitter : public BurstEmitter {
  std::mt19937 m_mt{std::random_device{}()};

public:
  PlayerMoveSparksEmitter(const DirectX::XMVECTOR &pos)
      : BurstEmitter(8, pos, 7) {}

protected:
  Particle *createParticle() override {
    std::uniform_real_distribution<float> off(-0.2f, 0.2f);
    std::uniform_real_distribution<float> spd(1.0f, 2.5f);
    std::uniform_real_distribution<float> life(0.15f, 0.3f);
    float s = spd(m_mt);
    auto spawn = DirectX::XMVectorAdd(
        GetPosition(), DirectX::XMVectorSet(off(m_mt), 0.0f, off(m_mt), 0.0f));
    auto vel = DirectX::XMVectorSet(off(m_mt) * 2.0f, s, off(m_mt) * 2.0f, 0.0f);
    return new BurstParticle(spawn, vel, life(m_mt), 0.05f,
                             {0.2f, 0.7f, 1.0f, 1.0f},
                             {0.1f, 0.4f, 0.8f, 1.0f});
  }
};
