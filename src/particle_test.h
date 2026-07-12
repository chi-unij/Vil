// ======================================
// File: particle_test.h
// Purpose: Particleの実装.
// Author : LEE CHEE HOW
// Date   : 2026/02/04
// DirectX 11ベースのパーティクルエフェクトファイルをDirectX 12に統合できるようリファクタリングしました。
// ======================================

#pragma once

#ifndef PARTICLE_TEST_H
#define PARTICLE_TEST_H

#include "particle.h"
#include <DirectXMath.h>
#include <random>

class NormalParticle : public Particle
{
private:
	float m_scale{ 1.0f };
	float m_alpha{ 1.0f };

public:
	NormalParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class NormalEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	NormalEmitter(size_t capacity, const DirectX::XMVECTOR& position, double particles_per_second, bool is_emmit)
		: Emitter(capacity, position, particles_per_second, is_emmit)
	{
	}

protected:
	Particle* createParticle() override;
};

// ---- Smoke ----

class SmokeParticle : public Particle
{
private:
	float m_scale{ 0.2f };
	float m_alpha{ 1.0f };

public:
	SmokeParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class SmokeEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	SmokeEmitter(size_t capacity, const DirectX::XMVECTOR& position, double particles_per_second, bool is_emmit)
		: Emitter(capacity, position, particles_per_second, is_emmit)
	{
	}

protected:
	Particle* createParticle() override;
};

// ---- Spark ----

class SparkParticle : public Particle
{
private:
	float m_scale{ 0.15f };
	float m_alpha{ 1.0f };

public:
	SparkParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class SparkEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	SparkEmitter(size_t capacity, const DirectX::XMVECTOR& position, double particles_per_second, bool is_emmit)
		: Emitter(capacity, position, particles_per_second, is_emmit)
	{
	}

protected:
	Particle* createParticle() override;
};

class SparkBurstEmitter : public BurstEmitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	SparkBurstEmitter(size_t capacity, const DirectX::XMVECTOR& position, size_t burstCount)
		: BurstEmitter(capacity, position, burstCount)
	{
	}

protected:
	Particle* createParticle() override;
};

class RiverMistParticle : public Particle
{
private:
	float m_scale{ 0.55f };
	float m_alpha{ 0.0f };

public:
	RiverMistParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class RiverMistEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	RiverMistEmitter(size_t capacity, const DirectX::XMVECTOR& position, double particles_per_second, bool is_emmit)
		: Emitter(capacity, position, particles_per_second, is_emmit)
	{
	}

protected:
	Particle* createParticle() override;
};

class ShrineFogParticle : public Particle
{
private:
	float m_scale{ 0.8f };
	float m_alpha{ 0.0f };

public:
	ShrineFogParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class ShrineFogEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };
	float m_side{ 1.0f };

public:
	ShrineFogEmitter(size_t capacity, const DirectX::XMVECTOR& position, float side,
	                 double particles_per_second, bool is_emmit)
		: Emitter(capacity, position, particles_per_second, is_emmit)
		, m_side(side)
	{
	}

	void WarmStart(double seconds = 5.0);

protected:
	Particle* createParticle() override;
};

class RiverElectricParticle : public Particle
{
private:
	float m_alpha{ 0.0f };
	float m_width{ 0.25f };
	float m_length{ 0.9f };
	float m_slant{ 0.0f };
	float m_intensity{ 1.0f };

public:
	RiverElectricParticle(const DirectX::XMVECTOR& position,
	                      const DirectX::XMVECTOR& velocity, double life_time,
	                      float width, float length, float slant,
	                      float intensity)
		: Particle(position, velocity, life_time)
		, m_width(width)
		, m_length(length)
		, m_slant(slant)
		, m_intensity(intensity) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class RiverElectricEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };
	float m_intensity{ 0.7f };

public:
	RiverElectricEmitter(size_t capacity, const DirectX::XMVECTOR& position)
		: Emitter(capacity, position, 42.0, false)
	{
	}

	void SetIntensity(float intensity);
	void Prime(double seconds = 0.18);

protected:
	Particle* createParticle() override;
};

class MirrorPickupBurstEmitter : public BurstEmitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	MirrorPickupBurstEmitter(size_t capacity,
	                        const DirectX::XMVECTOR& position,
	                        size_t burst_count)
		: BurstEmitter(capacity, position, burst_count)
	{
	}

protected:
	Particle* createParticle() override;
};

class MeteorFlameParticle : public Particle
{
private:
	float m_alpha{ 0.0f };
	float m_width{ 0.30f };
	float m_length{ 0.9f };
	float m_phase{ 0.0f };
	float m_intensity{ 1.0f };

public:
	MeteorFlameParticle(const DirectX::XMVECTOR& position,
	                    const DirectX::XMVECTOR& velocity, double life_time,
	                    float width, float length, float phase,
	                    float intensity)
		: Particle(position, velocity, life_time)
		, m_width(width)
		, m_length(length)
		, m_phase(phase)
		, m_intensity(intensity) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class MeteorFlameEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };
	float m_radius{ 3.0f };
	float m_intensity{ 0.5f };

public:
	MeteorFlameEmitter(size_t capacity, const DirectX::XMVECTOR& position)
		: Emitter(capacity, position, 80.0, false)
	{
	}

	void SetRadius(float radius);
	void SetIntensity(float intensity);

protected:
	Particle* createParticle() override;
};

class LineRiftParticle : public Particle
{
private:
	float m_alpha{ 0.0f };
	float m_width{ 0.20f };
	float m_length{ 1.0f };
	float m_slant{ 0.0f };
	float m_intensity{ 1.0f };

public:
	LineRiftParticle(const DirectX::XMVECTOR& position,
	                 const DirectX::XMVECTOR& velocity, double life_time,
	                 float width, float length, float slant,
	                 float intensity)
		: Particle(position, velocity, life_time)
		, m_width(width)
		, m_length(length)
		, m_slant(slant)
		, m_intensity(intensity) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class LineRiftEmitter : public Emitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };
	bool m_vertical{ true };
	float m_halfLength{ 3.5f };
	float m_halfWidth{ 1.4f };
	float m_intensity{ 0.5f };

public:
	LineRiftEmitter(size_t capacity, const DirectX::XMVECTOR& position)
		: Emitter(capacity, position, 90.0, false)
	{
	}

	void SetLine(bool vertical, float half_length, float half_width);
	void SetIntensity(float intensity);

protected:
	Particle* createParticle() override;
};

class MirrorSparkParticle : public Particle
{
private:
	float m_scale{ 0.20f };
	float m_alpha{ 1.0f };

public:
	MirrorSparkParticle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: Particle(position, velocity, life_time) {
	}

	void Update(double elapsed_time) override;
	ParticleVisual GetVisual() const override;
};

class MirrorSparkBurstEmitter : public BurstEmitter
{
private:
	std::mt19937 m_mt{ std::random_device{}() };

public:
	MirrorSparkBurstEmitter(size_t capacity, const DirectX::XMVECTOR& position, size_t burstCount)
		: BurstEmitter(capacity, position, burstCount)
	{
	}

protected:
	Particle* createParticle() override;
};

#endif // PARTICLE_TEST_H
