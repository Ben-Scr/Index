#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {
	class Scene;

	// AABB world-point query shared by editor and C# scripting. UI (RectTransform2D) hits always win over world-space hits. Caller must ensure TransformHierarchySystem::Propagate and ComputeUILayout have run first.
	class INDEX_API EntityPicker {
	public:
		static bool TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity);
	};
}
