/*==============================================================================

   Particle system base classes [particle.h]
                                         Author : LEE CHEE HOW
                                         Date   : 2026/02/04
   DirectX 11ベースのパーティクルエフェクトファイルをDirectX 12に統合できるようリファクタリングしました。
--------------------------------------------------------------------------------

==============================================================================*/
#ifndef PARTICLE_H
#define PARTICLE_H

#include <DirectXMath.h>

// Visual data exported by each particle for batch rendering (DX12).
struct ParticleVisual {
	DirectX::XMFLOAT3 position{};
	float scale = 1.0f;
	DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

class Particle
{
private:
	DirectX::XMVECTOR m_position{};
	DirectX::XMVECTOR m_velocity{};
	double m_life_time{};
	double m_accumulated_time{};

public:
	Particle(const DirectX::XMVECTOR& position, const DirectX::XMVECTOR& velocity, double life_time)
		: m_position(position)
		, m_velocity(velocity)
		, m_life_time(life_time) {
	}

	virtual ~Particle() = default;

	virtual bool IsDestroy() const {
		return m_life_time <= 0.0;
	}

	virtual void Update(double elapsed_time) {
		m_accumulated_time += elapsed_time;
		if (m_accumulated_time > m_life_time) {
			Destroy();
		}
	}

	// DX12: Particles export their visual data for batch rendering.
	virtual ParticleVisual GetVisual() const {
		ParticleVisual v{};
		DirectX::XMStoreFloat3(&v.position, m_position);
		return v;
	}

protected:
	virtual void Destroy() {
		m_life_time = 0.0;
	}

	void SetPosition(const DirectX::XMVECTOR& position) {
		m_position = position;
	}

	void SetVelocity(const DirectX::XMVECTOR& velocity) {
		m_velocity = velocity;
	}

	const DirectX::XMVECTOR& GetPosition() const {
		return m_position;
	}

	const DirectX::XMVECTOR& GetVelocity() const {
		return m_velocity;
	}

	void AddPosition(const DirectX::XMVECTOR& v) {
		m_position = DirectX::XMVectorAdd(m_position, v);
	}

	void AddVelocity(const DirectX::XMVECTOR& v) {
		m_velocity = DirectX::XMVectorAdd(m_velocity, v);
	}

	double GetAccumulatedTime() const {
		return m_accumulated_time;
	}

	double GetLifeTime() const {
		return m_life_time;
	}
};

class Emitter
{
private:
	DirectX::XMVECTOR m_position{};
	double m_particles_per_second{};
	double m_accumulated_time = 0.0;
	bool m_is_emmit{};
	size_t m_capacity{};
	size_t m_count{};
	Particle** m_particles{};

protected:
	virtual Particle* createParticle() = 0;

	// Phase 8A: spawn a particle into the pool if capacity allows.
	bool SpawnParticle() {
		if (m_count >= m_capacity) return false;
		m_particles[m_count++] = createParticle();
		return true;
	}

	const DirectX::XMVECTOR& GetPosition() const {
		return m_position;
	}

	double GetParticlesPerSecond() const {
		return m_particles_per_second;
	}

	double GetAccumulatedTime() const {
		return m_accumulated_time;
	}

	void SetParticlesPerSecond(double particles_per_second) {
		m_particles_per_second = particles_per_second;
	}

public:
	Emitter(size_t capacity, const DirectX::XMVECTOR& position, double paricles_per_second, bool is_emmit)
		: m_position(position)
		, m_particles_per_second(paricles_per_second)
		, m_is_emmit(is_emmit)
		, m_capacity(capacity)
	{
		m_particles = new Particle * [m_capacity];
	}

	virtual ~Emitter() {
		for (size_t i = 0; i < m_count; ++i)
			delete m_particles[i];
		delete[] m_particles;
	}

	virtual void Update(double elapsed_time);

	void Emmit(bool isEmmit) { m_is_emmit = isEmmit; }
	bool isEmmit() const { return m_is_emmit; }

	// DX12: Public setter so emitter can follow cursor.
	void SetPosition(const DirectX::XMVECTOR& position) {
		m_position = position;
	}

	// DX12: Accessors for batch rendering (ParticleRenderer iterates particles).
	size_t GetCount() const { return m_count; }
	const Particle* GetParticle(size_t i) const { return m_particles[i]; }

	// Phase 8A: capacity query helpers.
	bool IsFull() const { return m_count >= m_capacity; }
	size_t GetCapacity() const { return m_capacity; }
};

// Phase 8A: one-shot burst emitter — spawns N particles at once, then dies.
class BurstEmitter : public Emitter {
	size_t m_burstCount{};
	bool m_fired = false;

public:
	BurstEmitter(size_t capacity, const DirectX::XMVECTOR& position, size_t burstCount)
		: Emitter(capacity, position, 0.0, false)
		, m_burstCount(burstCount)
	{}

	// Trigger burst at given position.
	void Fire(const DirectX::XMVECTOR& position) {
		SetPosition(position);
		m_fired = false; // allow re-fire
	}

	void Update(double elapsed_time) override {
		if (!m_fired) {
			m_fired = true;
			for (size_t i = 0; i < m_burstCount; ++i)
				SpawnParticle();
		}
		Emitter::Update(elapsed_time);
	}

	bool IsFinished() const {
		return m_fired && GetCount() == 0;
	}
};

#endif // PARTICLE_H
