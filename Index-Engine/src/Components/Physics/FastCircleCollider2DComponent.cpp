#include "pch.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
#include <cmath>

namespace Index {
	namespace {
		const Transform2DComponent* TryGetTransform(const Scene& scene, EntityHandle entity) {
			if (entity == entt::null || !scene.IsValid(entity) || !scene.HasComponent<Transform2DComponent>(entity)) {
				return nullptr;
			}
			return &scene.GetComponent<Transform2DComponent>(entity);
		}

		float UniformScaleFromTransform(const Transform2DComponent* tr) {
			if (!tr) return 1.0f;
			// A circle has a single radius — there's no honest way to
			// represent non-uniform x/y scaling. Take the larger absolute
			// axis so collisions don't silently shrink when the user
			// stretches the entity along just one axis.
			return std::max(std::abs(tr->Scale.x), std::abs(tr->Scale.y));
		}
	}

	void FastCircleCollider2DComponent::SetRadius(float r) {
		Radius = r;
		if (m_Collider) {
			m_Collider->SetRadius(r * m_LastAppliedScale);
		}
	}

	void FastCircleCollider2DComponent::SyncWithTransform(const Scene& scene) {
		if (!m_Collider) return;
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle);
		const float scale = UniformScaleFromTransform(tr);
		if (scale == m_LastAppliedScale) return;
		m_Collider->SetRadius(Radius * scale);
		m_LastAppliedScale = scale;
	}
}
