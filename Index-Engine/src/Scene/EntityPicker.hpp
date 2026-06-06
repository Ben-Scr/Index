#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Collections/AABB.hpp"
#include "Scene/EntityHandle.hpp"

#include <vector>

namespace Index {
	class Scene;

	// AABB world-point query shared by editor and C# scripting. UI (RectTransform2D) hits always win over world-space hits. Caller must ensure TransformHierarchySystem::Propagate and ComputeUILayout have run first.
	class INDEX_API EntityPicker {
	public:
		static bool TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity);

		// Marquee query: append every entity whose bounds intersect the world-space
		// rectangle to outEntities. Same bounds/space rules as TryPickEntity.
		static void PickEntitiesInRect(Scene& scene, const AABB& worldRect, std::vector<EntityHandle>& outEntities);
	};
}
