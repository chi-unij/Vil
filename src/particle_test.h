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

#endif // PARTICLE_TEST_H
