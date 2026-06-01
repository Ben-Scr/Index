#pragma once
#include "Collections/AABB.hpp"

namespace Index {
	// Renderer2D transform/AABB cache for StaticTag entities. Cleared via on_destroy<StaticTag> observer so retoggling the tag starts clean.
	struct StaticRenderData {
		AABB CachedAABB{};
		Vec2 CachedPosition{};
		Vec2 CachedScale{};
		float CachedRotation = 0.0f;
		bool Valid = false;
	};
}
