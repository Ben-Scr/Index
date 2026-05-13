#pragma once

#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {
	class Scene;

	struct RaycastHit2D {
		EntityHandle entity;
		Scene* scene = nullptr;
		Vec2 point;
		Vec2 normal;
		float distance;
	};

} // namespace Index
