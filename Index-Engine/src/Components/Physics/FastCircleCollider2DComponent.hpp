#pragma once
#include "Core/Export.hpp"
#include "Physics/IndexPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <CircleCollider.hpp>

namespace Index {

	class Scene;

	/// Circle collider using Index-Physics. Radius is scaled by max(|Scale.x|, |Scale.y|) — a circle can't represent non-uniform scale so max avoids silent shrinkage.
	struct INDEX_API FastCircleCollider2DComponent {
		float Radius = 0.5f;

		// Runtime pointer — set by scene hooks, not serialized
		IndexPhys::CircleCollider* m_Collider = nullptr;
		float m_LastAppliedScale = 1.0f;
		EntityHandle m_EntityHandle = entt::null;

		bool IsValid() const { return m_Collider != nullptr; }

		float GetRadius() const {
			return m_Collider ? m_Collider->GetRadius() : Radius;
		}

		void SetRadius(float r);

		void SyncWithTransform(const Scene& scene);
	};

}
