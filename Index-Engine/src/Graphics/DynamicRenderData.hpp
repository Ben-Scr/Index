#pragma once
#include "Collections/AABB.hpp"

namespace Index {
	// Separate from Transform2DComponent to keep Transform's cache line small; cleared on SpriteRendererComponent destroy to prevent stale bounds on reused entity slots.
	struct DynamicRenderData {
		AABB CachedAABB{};
		Vec2 CachedPosition{};
		Vec2 CachedScale{};
		float CachedRotation = 0.0f;
		bool Valid = false;
	};
}
