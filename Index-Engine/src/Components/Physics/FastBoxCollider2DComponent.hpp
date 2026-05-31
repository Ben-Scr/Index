#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Physics/IndexPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <BoxCollider.hpp>

namespace Index {

	class Scene;

	/// Box collider component using the Axiom-Physics library.
	/// Provides simple AABB-based collision detection. For polygon-based collision
	/// with friction and restitution, use the standard BoxCollider2DComponent instead.
	///
	/// The authored HalfExtents are multiplied by the owning entity's
	/// Transform2D.Scale before being pushed to the underlying AxiomPhys collider,
	/// matching the convention BoxCollider2DComponent already uses for Box2D —
	/// resize the transform and the collider follows.
	struct INDEX_API FastBoxCollider2DComponent {
		Vec2 HalfExtents{ 0.5f, 0.5f };

		// Runtime pointer — set by scene hooks, not serialized
		AxiomPhys::BoxCollider* m_Collider = nullptr;
		// Last-applied transform scale; lets PhysicsSystem2D's per-frame
		// sync short-circuit when nothing changed.
		Vec2 m_LastAppliedScale{ 1.0f, 1.0f };
		EntityHandle m_EntityHandle = entt::null;

		bool IsValid() const { return m_Collider != nullptr; }

		Vec2 GetHalfExtents() const {
			if (m_Collider) {
				auto he = m_Collider->GetHalfExtents();
				return { he.x, he.y };
			}
			return HalfExtents;
		}

		void SetHalfExtents(const Vec2& he);

		// Push (HalfExtents × Transform2D.Scale) into the underlying
		// AxiomPhys collider. Called by PhysicsSystem2D when the entity's
		// transform is dirty so a scale edit propagates immediately.
		void SyncWithTransform(const Scene& scene);
	};

}
