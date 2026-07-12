// ======================================
// File: particle_test.cpp
// Purpose: Particleの実装.
// Author : LEE CHEE HOW
// Date   : 2026/02/04
// DirectX 11ベースのパーティクルエフェクトファイルをDirectX 12に統合できるようリファクタリングしました。
// ======================================

#include "particle_test.h"
#include <algorithm>
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

Particle* SparkBurstEmitter::createParticle()
{
	std::uniform_real_distribution<float> angle_dist{ -DirectX::XM_PI, DirectX::XM_PI };
	std::uniform_real_distribution<float> ring_dist{ 0.0f, 1.0f };
	std::uniform_real_distribution<float> elev_dist{ 0.18f, 0.82f };
	std::uniform_real_distribution<float> speed_dist{ 4.2f, 8.4f };
	std::uniform_real_distribution<float> lifetime_dist{ 0.42f, 0.95f };

	const float angle = angle_dist(m_mt);
	const float ring = ring_dist(m_mt);
	const float elevation = elev_dist(m_mt);
	const float speed = speed_dist(m_mt);

	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(cosf(angle) * ring * 0.52f, 0.04f,
		            sinf(angle) * ring * 0.52f, 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		cosf(angle) * speed * (1.0f - elevation * 0.45f),
		speed * elevation,
		sinf(angle) * speed * (1.0f - elevation * 0.45f),
		0.0f);

	return new SparkParticle(spawnPos, velocity, lifetime_dist(m_mt));
}

void RiverMistParticle::Update(double elapsed_time)
{
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	const float fadeIn = std::min(ratio * 3.0f, 1.0f);
	const float fadeOut = 1.0f - ratio;

	m_scale = 0.48f + ratio * 1.35f;
	m_alpha = fadeIn * fadeOut * 0.38f;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, 0.035f, 0.0f, 0.0f), static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual RiverMistParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;
	v.color = XMFLOAT4{ 0.58f, 0.95f, 1.0f, m_alpha };
	return v;
}

Particle* RiverMistEmitter::createParticle()
{
	std::uniform_real_distribution<float> offset_x{ -1.55f, 1.55f };
	std::uniform_real_distribution<float> offset_z{ -0.42f, 0.42f };
	std::uniform_real_distribution<float> drift_x{ -0.045f, 0.045f };
	std::uniform_real_distribution<float> drift_z{ -0.030f, 0.030f };
	std::uniform_real_distribution<float> up_speed{ 0.035f, 0.12f };
	std::uniform_real_distribution<float> lifetime{ 2.8f, 4.8f };

	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(offset_x(m_mt), 0.0f, offset_z(m_mt), 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		drift_x(m_mt), up_speed(m_mt), drift_z(m_mt), 0.0f);

	return new RiverMistParticle(spawnPos, velocity, lifetime(m_mt));
}

void ShrineFogParticle::Update(double elapsed_time)
{
	const float ratio = static_cast<float>(
		std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	const float fadeIn = std::min(ratio * 4.0f, 1.0f);
	const float fadeOut = 1.0f - std::clamp((ratio - 0.55f) / 0.45f, 0.0f, 1.0f);

	m_scale = 0.78f + ratio * 1.85f;
	m_alpha = fadeIn * fadeOut * 0.26f;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(
		XMVectorSet(0.0f, 0.022f, 0.0f, 0.0f),
		static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual ShrineFogParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;
	v.size = XMFLOAT2{ m_scale * 1.50f, m_scale * 0.78f };
	v.color = XMFLOAT4{ 0.38f, 0.43f, 0.44f, m_alpha };
	return v;
}

void ShrineFogEmitter::WarmStart(double seconds)
{
	constexpr double step = 1.0 / 60.0;
	for (double elapsed = 0.0; elapsed < seconds; elapsed += step)
		Update(step);
}

Particle* ShrineFogEmitter::createParticle()
{
	std::uniform_real_distribution<float> offset_x{ -5.4f, 5.4f };
	std::uniform_real_distribution<float> offset_z{ -3.6f, 3.6f };
	std::uniform_real_distribution<float> drift_x{ -0.14f, 0.14f };
	std::uniform_real_distribution<float> drift_z{ -0.20f, 0.20f };
	std::uniform_real_distribution<float> up_speed{ 0.045f, 0.16f };
	std::uniform_real_distribution<float> lifetime{ 5.2f, 7.4f };

	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(offset_x(m_mt), 0.0f, offset_z(m_mt), 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		drift_x(m_mt) - m_side * 0.055f, up_speed(m_mt), drift_z(m_mt), 0.0f);

	return new ShrineFogParticle(spawnPos, velocity, lifetime(m_mt));
}

void RiverElectricParticle::Update(double elapsed_time)
{
	const float ratio = static_cast<float>(
		std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	const float envelope = std::sin(ratio * XM_PI);
	const float flicker = 0.72f + 0.28f *
		std::sin(static_cast<float>(GetAccumulatedTime()) * 92.0f +
		         m_slant * 13.0f);
	m_alpha = envelope * flicker * (0.58f + m_intensity * 0.28f);

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	Particle::Update(elapsed_time);
}

ParticleVisual RiverElectricParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.size = XMFLOAT2{ m_width, m_length };
	v.color = XMFLOAT4{ 0.20f, 0.82f, 1.0f, m_alpha };
	v.shape = 2.0f;
	v.slant = m_slant;
	return v;
}

void RiverElectricEmitter::SetIntensity(float intensity)
{
	m_intensity = std::clamp(intensity, 0.0f, 1.5f);
	SetParticlesPerSecond(14.0 + 42.0 * static_cast<double>(m_intensity));
}

void RiverElectricEmitter::Prime(double seconds)
{
	constexpr double step = 1.0 / 60.0;
	for (double elapsed = 0.0; elapsed < seconds; elapsed += step)
		Update(step);
}

Particle* RiverElectricEmitter::createParticle()
{
	std::uniform_real_distribution<float> offset_x{ -4.0f, 4.0f };
	std::uniform_real_distribution<float> offset_y{ 0.02f, 0.12f };
	std::uniform_real_distribution<float> offset_z{ -2.8f, 2.8f };
	std::uniform_real_distribution<float> drift_x{ -0.32f, 0.32f };
	std::uniform_real_distribution<float> drift_z{ -0.48f, 0.48f };
	std::uniform_real_distribution<float> lifetime{ 0.18f, 0.48f };
	std::uniform_real_distribution<float> width{ 0.18f, 0.34f };
	std::uniform_real_distribution<float> length{ 0.55f, 1.25f };
	std::uniform_real_distribution<float> slant{ -0.85f, 0.85f };

	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(offset_x(m_mt), offset_y(m_mt), offset_z(m_mt), 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		drift_x(m_mt), 0.02f, drift_z(m_mt), 0.0f);

	return new RiverElectricParticle(spawnPos, velocity, lifetime(m_mt),
	                                 width(m_mt), length(m_mt), slant(m_mt),
	                                 m_intensity);
}

Particle* MirrorPickupBurstEmitter::createParticle()
{
	std::uniform_real_distribution<float> angle{ -XM_PI, XM_PI };
	std::uniform_real_distribution<float> elevation{ 0.12f, 0.78f };
	std::uniform_real_distribution<float> radius{ 0.0f, 0.28f };
	std::uniform_real_distribution<float> speed{ 2.8f, 7.2f };
	std::uniform_real_distribution<float> lifetime{ 0.22f, 0.55f };
	std::uniform_real_distribution<float> width{ 0.12f, 0.24f };
	std::uniform_real_distribution<float> length{ 0.38f, 0.86f };
	std::uniform_real_distribution<float> slant{ -0.95f, 0.95f };

	const float a = angle(m_mt);
	const float e = elevation(m_mt);
	const float r = radius(m_mt);
	const float v = speed(m_mt);
	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(), XMVectorSet(std::cos(a) * r, 0.0f,
		                           std::sin(a) * r, 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		std::cos(a) * v * (1.0f - e * 0.35f), v * e,
		std::sin(a) * v * (1.0f - e * 0.35f), 0.0f);

	return new RiverElectricParticle(spawnPos, velocity, lifetime(m_mt),
	                                 width(m_mt), length(m_mt), slant(m_mt),
	                                 1.25f);
}

void MeteorFlameParticle::Update(double elapsed_time)
{
	const float ratio = static_cast<float>(
		std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	const float fadeIn = std::min(ratio * 7.0f, 1.0f);
	const float fadeOut = 1.0f - std::clamp((ratio - 0.52f) / 0.48f, 0.0f, 1.0f);
	m_alpha = fadeIn * fadeOut * (0.62f + m_intensity * 0.24f);
	m_width *= 1.0f + static_cast<float>(elapsed_time) * 0.18f;
	m_length *= 1.0f + static_cast<float>(elapsed_time) * 0.32f;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, 0.55f, 0.0f, 0.0f),
	                          static_cast<float>(elapsed_time)));
	Particle::Update(elapsed_time);
}

ParticleVisual MeteorFlameParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.size = XMFLOAT2{ m_width, m_length };
	const float ratio = static_cast<float>(
		std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	v.color = XMFLOAT4{ 1.0f, 0.68f - ratio * 0.42f,
	                   0.10f - ratio * 0.07f, m_alpha };
	v.shape = 3.0f;
	v.slant = m_phase;
	return v;
}

void MeteorFlameEmitter::SetRadius(float radius)
{
	m_radius = std::max(radius, 0.1f);
}

void MeteorFlameEmitter::SetIntensity(float intensity)
{
	m_intensity = std::clamp(intensity, 0.0f, 1.5f);
	SetParticlesPerSecond(28.0 + 104.0 * static_cast<double>(m_intensity));
}

Particle* MeteorFlameEmitter::createParticle()
{
	std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
	std::uniform_real_distribution<float> angle{ -XM_PI, XM_PI };
	std::uniform_real_distribution<float> up_speed{ 1.2f, 3.4f };
	std::uniform_real_distribution<float> drift{ -0.34f, 0.34f };
	std::uniform_real_distribution<float> lifetime{ 0.42f, 0.92f };
	std::uniform_real_distribution<float> width{ 0.20f, 0.42f };
	std::uniform_real_distribution<float> length{ 0.58f, 1.42f };
	std::uniform_real_distribution<float> phase{ -XM_PI, XM_PI };

	const float a = angle(m_mt);
	const float r = std::sqrt(unit(m_mt)) * m_radius;
	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(std::cos(a) * r, 0.04f, std::sin(a) * r, 0.0f));
	const XMVECTOR velocity =
		XMVectorSet(drift(m_mt), up_speed(m_mt), drift(m_mt), 0.0f);

	return new MeteorFlameParticle(spawnPos, velocity, lifetime(m_mt),
	                               width(m_mt), length(m_mt), phase(m_mt),
	                               m_intensity);
}

void LineRiftParticle::Update(double elapsed_time)
{
	const float ratio = static_cast<float>(
		std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	const float fadeIn = std::min(ratio * 8.0f, 1.0f);
	const float fadeOut = 1.0f - std::clamp((ratio - 0.46f) / 0.54f, 0.0f, 1.0f);
	const float flicker = 0.68f + 0.32f *
		std::sin(static_cast<float>(GetAccumulatedTime()) * 76.0f +
		         m_slant * 17.0f);
	m_alpha = fadeIn * fadeOut * flicker * (0.62f + m_intensity * 0.25f);
	m_length *= 1.0f + static_cast<float>(elapsed_time) * 0.45f;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, 0.28f, 0.0f, 0.0f),
	                          static_cast<float>(elapsed_time)));
	Particle::Update(elapsed_time);
}

ParticleVisual LineRiftParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.size = XMFLOAT2{ m_width, m_length };
	v.color = XMFLOAT4{ 1.0f, 0.08f, 0.30f, m_alpha };
	v.shape = 4.0f;
	v.slant = m_slant;
	return v;
}

void LineRiftEmitter::SetLine(bool vertical, float half_length,
	                            float half_width)
{
	m_vertical = vertical;
	m_halfLength = std::max(half_length, 0.1f);
	m_halfWidth = std::max(half_width, 0.05f);
}

void LineRiftEmitter::SetIntensity(float intensity)
{
	m_intensity = std::clamp(intensity, 0.0f, 1.5f);
	SetParticlesPerSecond(24.0 + 148.0 * static_cast<double>(m_intensity));
}

Particle* LineRiftEmitter::createParticle()
{
	std::uniform_real_distribution<float> along{ -m_halfLength, m_halfLength };
	std::uniform_real_distribution<float> across{ -m_halfWidth, m_halfWidth };
	std::uniform_real_distribution<float> height{ 0.02f, 0.12f };
	std::uniform_real_distribution<float> up_speed{ 1.8f, 5.0f };
	std::uniform_real_distribution<float> drift{ -0.28f, 0.28f };
	std::uniform_real_distribution<float> lifetime{ 0.26f, 0.72f };
	std::uniform_real_distribution<float> width{ 0.12f, 0.28f };
	std::uniform_real_distribution<float> length{ 0.48f, 1.55f };
	std::uniform_real_distribution<float> slant{ -0.72f, 0.72f };

	const float lineOffset = along(m_mt);
	const float widthOffset = across(m_mt);
	const XMVECTOR localOffset =
		m_vertical
			? XMVectorSet(widthOffset, height(m_mt), lineOffset, 0.0f)
			: XMVectorSet(lineOffset, height(m_mt), widthOffset, 0.0f);
	const XMVECTOR spawnPos = XMVectorAdd(GetPosition(), localOffset);
	const XMVECTOR velocity =
		XMVectorSet(drift(m_mt), up_speed(m_mt), drift(m_mt), 0.0f);

	return new LineRiftParticle(spawnPos, velocity, lifetime(m_mt),
	                            width(m_mt), length(m_mt), slant(m_mt),
	                            m_intensity);
}

void MirrorSparkParticle::Update(double elapsed_time)
{
	float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));

	m_scale = 0.20f - ratio * 0.17f;
	m_alpha = 1.0f - ratio;

	AddPosition(XMVectorScale(GetVelocity(), static_cast<float>(elapsed_time)));
	AddVelocity(XMVectorScale(XMVectorSet(0.0f, -3.2f, 0.0f, 0.0f), static_cast<float>(elapsed_time)));

	Particle::Update(elapsed_time);
}

ParticleVisual MirrorSparkParticle::GetVisual() const
{
	ParticleVisual v{};
	XMStoreFloat3(&v.position, GetPosition());
	v.scale = m_scale;
	const float ratio = static_cast<float>(std::min(GetAccumulatedTime() / GetLifeTime(), 1.0));
	v.color = XMFLOAT4{ 0.42f + ratio * 0.36f, 1.0f, 0.92f, m_alpha };
	return v;
}

Particle* MirrorSparkBurstEmitter::createParticle()
{
	std::uniform_real_distribution<float> angle_dist{ -DirectX::XM_PI, DirectX::XM_PI };
	std::uniform_real_distribution<float> ring_dist{ 0.1f, 1.0f };
	std::uniform_real_distribution<float> elev_dist{ 0.18f, 0.92f };
	std::uniform_real_distribution<float> speed_dist{ 5.2f, 11.5f };
	std::uniform_real_distribution<float> lifetime_dist{ 0.34f, 0.82f };

	const float angle = angle_dist(m_mt);
	const float ring = ring_dist(m_mt);
	const float elevation = elev_dist(m_mt);
	const float speed = speed_dist(m_mt);

	const XMVECTOR spawnPos = XMVectorAdd(
		GetPosition(),
		XMVectorSet(cosf(angle) * ring * 0.76f, 0.10f,
		            sinf(angle) * ring * 0.76f, 0.0f));
	const XMVECTOR velocity = XMVectorSet(
		cosf(angle) * speed * (0.55f + elevation * 0.25f),
		speed * elevation,
		sinf(angle) * speed * (0.55f + elevation * 0.25f),
		0.0f);

	return new MirrorSparkParticle(spawnPos, velocity, lifetime_dist(m_mt));
}
