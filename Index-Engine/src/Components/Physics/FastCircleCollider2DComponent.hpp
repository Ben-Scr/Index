#pragma once
#include "Core/Export.hpp"
#include "Physics/IndexPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <CircleCollider.hpp>

namespace Index {

	class Scene;

	/// Circle collider component using the Axiom-Physics library.
	///
	/// The authored Radius is multiplied by the larger absolute axis of the
	/// owning entity's Transform2D.Scale before being pushed to the underlying
	/// AxiomPhys circle (a single radius can't represent non-uniform scaling;
	/// taking the max keeps collisions from silently shrinking). Matches the
	/// scale-follows-transform convention BoxCollider2DComponent already uses.
	struct INDEX_API FastCircleCollider2DComponent {
		float Radius = 0.5f;

		// Runtime pointer — set by scene hooks, not serialized
		AxiomPhys::CircleCollider* m_Collider = nullptr;
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
