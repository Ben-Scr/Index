#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Collections/AABB.hpp"
#include "Scene/EntityHandle.hpp"

#include <vector>

namespace Index {
	class Scene;

	// Precise world-point entity query shared by editor and C# scripting. Hits are
	// tested against each entity's rotated/scaled quad (an oriented box, not a loose
	// AABB); with pixelPrecise set, sprites are additionally alpha-tested against
	// their texture so clicks on transparent texels fall through. UI (RectTransform2D)
	// hits always win over world-space hits. Caller must ensure
	// TransformHierarchySystem::Propagate and ComputeUILayout have run first.
	class INDEX_API EntityPicker {
	public:
		static bool TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity,
			bool pixelPrecise = false);

		// Every entity whose precise bounds contain worldPoint, sorted topmost-first
		// (UI above world; then sorting layer/order; later-created breaks ties). Lets
		// the editor cycle selection through entities stacked under the cursor.
		static void PickEntitiesAtPoint(Scene& scene, const Vec2& worldPoint,
			std::vector<EntityHandle>& outEntities, bool pixelPrecise = false);

		// Marquee query: append every entity whose bounds intersect the world-space
		// rectangle to outEntities. Uses loose AABB bounds (box-select semantics).
		static void PickEntitiesInRect(Scene& scene, const AABB& worldRect, std::vector<EntityHandle>& outEntities);
	};
}
