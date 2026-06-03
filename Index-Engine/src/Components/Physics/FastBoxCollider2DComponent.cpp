#include "pch.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Scene/Scene.hpp"

namespace Index {
	namespace {
		const Transform2DComponent* TryGetTransform(const Scene& scene, EntityHandle entity) {
			if (entity == entt::null || !scene.IsValid(entity) || !scene.HasComponent<Transform2DComponent>(entity)) {
				return nullptr;
			}
			return &scene.GetComponent<Transform2DComponent>(entity);
		}
	}

	void FastBoxCollider2DComponent::SetHalfExtents(const Vec2& he) {
		HalfExtents = he;
		// The underlying IndexPhys collider is in world units, so SetHalfExtents
		// has to multiply by the entity's current Transform2D.Scale. SyncWithTransform
		// re-applies this whenever the transform's scale changes.
		if (m_Collider) {
			m_Collider->SetHalfExtents({ he.x * m_LastAppliedScale.x, he.y * m_LastAppliedScale.y });
		}
	}

	void FastBoxCollider2DComponent::SyncWithTransform(const Scene& scene) {
		if (!m_Collider) return;
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle);
		const Vec2 scale = tr ? tr->Scale : Vec2{ 1.0f, 1.0f };
		if (scale == m_LastAppliedScale) return;
		m_Collider->SetHalfExtents({ HalfExtents.x * scale.x, HalfExtents.y * scale.y });
		m_LastAppliedScale = scale;
	}
}
