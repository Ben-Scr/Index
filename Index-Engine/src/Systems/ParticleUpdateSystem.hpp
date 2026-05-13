#pragma once
#include "Scene/ISystem.hpp"

namespace Index {
	class ParticleUpdateSystem : public ISystem {
	public:
		virtual void Awake(Scene& scene);
		virtual void Update(Scene& scene);
	};
}