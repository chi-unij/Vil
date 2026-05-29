// ======================================
// File: particle_test.cpp
// Purpose: Particleの実装.
// Author : LEE CHEE HOW
// Date   : 2026/02/04
// DirectX 11ベースのパーティクルエフェクトファイルをDirectX 12に統合できるようリファクタリングしました。
// ======================================

#include "particle_test.h"
using namespace DirectX;

void NormalParticle::Update(double elapsed_time)
{
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));

	m_scale = (1.0f - ratio) * 0.5f;  // shrink over lifetime
	m_alpha = (1.0f - ratio);          // fade out

	// Move by velocity
	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	// Apply gravity
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, -2.0f, 0.0f, 0.0f), static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual NormalParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;
	v.color = XMFLOAT4{ 0.3f, 1.0f, 1.0f, m_alpha }; // cyan glow
	return v;
}

Particle* NormalEmitter::createParticle()
{
	std::uniform_real_distribution<float> angle_dist{ -DirectX::XM_PI, DirectX::XM_PI };
	std::uniform_real_distribution<float> radius_dist{ 0.2f, 0.5f };
	std::uniform_real_distribution<float> speed_dist{ 0.5f, 1.5f };
	std::uniform_real_distribution<float> lifetime_dist{ 0.5f, 1.2f };

	float angle = angle_dist(m_mt);
	float radius = radius_dist(m_mt);

	// Spawn on a circle around the emitter position
	XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(cosf(angle) * radius, 0.0f, sinf(angle) * radius, 0.0f)
	);

	// Slight outward + upward velocity for fountain/ring effect
	float speed = speed_dist(m_mt);
	XMVECTOR velocity = XMVectorSet(
		cosf(angle) * speed * 0.3f,
		speed,
		sinf(angle) * speed * 0.3f,
		0.0f
	);

	return new NormalParticle(spawnPos, velocity, lifetime_dist(m_mt));
}

// ============================================================================
// SmokeParticle — grows over lifetime, drifts upward, fades out
// ============================================================================

void SmokeParticle::Update(double elapsed_time)
{
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));

	m_scale = 0.2f + ratio * 1.3f;  // grow 0.2 -> 1.5
	m_alpha = 1.0f - ratio;          // fade out

	// Drift upward (slight negative gravity = buoyancy)
	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, 0.3f, 0.0f, 0.0f), static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual SmokeParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;
	v.color = XMFLOAT4{ 0.5f, 0.5f, 0.55f, m_alpha * 0.6f };
	return v;
}

Particle* SmokeEmitter::createParticle()
{
	std::uniform_real_distribution<float> offset_dist{ -0.3f, 0.3f };
	std::uniform_real_distribution<float> speed_dist{ 0.3f, 0.8f };
	std::uniform_real_distribution<float> drift_dist{ -0.15f, 0.15f };
	std::uniform_real_distribution<float> lifetime_dist{ 2.0f, 4.0f };

	XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(offset_dist(m_mt), 0.0f, offset_dist(m_mt), 0.0f)
	);

	float upSpeed = speed_dist(m_mt);
	XMVECTOR velocity = XMVectorSet(
		drift_dist(m_mt),
		upSpeed,
		drift_dist(m_mt),
		0.0f
	);

	return new SmokeParticle(spawnPos, velocity, lifetime_dist(m_mt));
}

// ============================================================================
// SparkParticle — small, shrinks, orange->red color shift, strong gravity
// ============================================================================

void SparkParticle::Update(double elapsed_time)
{
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));

	m_scale = 0.15f - ratio * 0.13f; // shrink 0.15 -> 0.02
	m_alpha = 1.0f - ratio;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	// Strong gravity
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, -9.8f, 0.0f, 0.0f), static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual SparkParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;

	// Orange-yellow (1.0, 0.8, 0.2) -> red (1.0, 0.2, 0.05) -> fade
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	float r = 1.0f;
	float g = 0.8f - ratio * 0.6f;  // 0.8 -> 0.2
	float b = 0.2f - ratio * 0.15f; // 0.2 -> 0.05
	v.color = XMFLOAT4{ r, g, b, m_alpha };
	return v;
}

Particle* SparkEmitter::createParticle()
{
	std::uniform_real_distribution<float> angle_dist{ -DirectX::XM_PI, DirectX::XM_PI };
	std::uniform_real_distribution<float> elev_dist{ 0.3f, 1.0f };
	std::uniform_real_distribution<float> speed_dist{ 2.0f, 5.0f };
	std::uniform_real_distribution<float> lifetime_dist{ 0.3f, 0.8f };

	float angle = angle_dist(m_mt);
	float elevation = elev_dist(m_mt);
	float speed = speed_dist(m_mt);

	XMVECTOR spawnPos = GetPosition();

	// Fast random directions, biased upward
	XMVECTOR velocity = XMVectorSet(
		cosf(angle) * speed * (1.0f - elevation),
		speed * elevation,
		sinf(angle) * speed * (1.0f - elevation),
		0.0f
	);

	return new SparkParticle(spawnPos, velocity, lifetime_dist(m_mt));
}
