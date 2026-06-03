#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Physics/IndexPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <BoxCollider.hpp>

namespace Index {

	class Scene;

	/// Box collider using Index-Physics (AABB only; no friction/restitution). HalfExtents are multiplied by Transform2D.Scale before upload.
	struct INDEX_API FastBoxCollider2DComponent {
		Vec2 HalfExtents{ 0.5f, 0.5f };

		// Runtime pointer — set by scene hooks, not serialized
		IndexPhys::BoxCollider* m_Collider = nullptr;
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

		void SyncWithTransform(const Scene& scene);
	};

}
