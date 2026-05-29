// ======================================
// File: particle.cpp
// Purpose: Emitter update logic (spawn, update, destroy particles).
// Author : LEE CHEE HOW
// Date   : 2026/02/04
// Refactored from DirectX 11 to DirectX 12 batch rendering.
// ======================================

#include "particle.h"
using namespace DirectX;

void Emitter::Update(double elapsed_time)
{
	m_accumulated_time += elapsed_time;

	const double sec_per_particle = 1.0 / m_particles_per_second;

	// Spawn new particles
	while (m_accumulated_time >= sec_per_particle)
	{
		if (m_count >= m_capacity) break;
		if (m_is_emmit) {
			m_particles[m_count++] = createParticle();
		}
		m_accumulated_time -= sec_per_particle;
	}

	// Update all particles
	for (size_t i = 0; i < m_count; i++) {
		m_particles[i]->Update(elapsed_time);
	}

	// Remove dead particles (swap with last)
	for (int i = static_cast<int>(m_count) - 1; i >= 0; i--) {
		if (m_particles[i]->IsDestroy()) {
			delete m_particles[i];
			m_particles[i] = m_particles[--m_count];
		}
	}
}
