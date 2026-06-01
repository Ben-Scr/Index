#pragma once

#include "Core/Export.hpp"

namespace Index {

	// Swappable physics boundary; lifecycle + tick only — components/systems still talk to PhysicsSystem2D directly until PhysicsBodyHandle (v2) plugs that leak.
	class INDEX_API IPhysicsEngine {
	public:
		virtual ~IPhysicsEngine() = default;

		virtual void Initialize() = 0;
		virtual void Shutdown() = 0;

		// Fixed-timestep simulation step. Called by Application::FixedUpdate.
		virtual void FixedUpdate(float dt) = 0;

		// Editor-mode + play-mode sync; no simulation step. Called every frame.
		virtual void Update() = 0;
	};

}
