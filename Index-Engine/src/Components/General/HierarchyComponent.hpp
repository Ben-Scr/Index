#pragma once
#include "Scene/EntityHandle.hpp"

#include <vector>

namespace Index {

	struct HierarchyComponent {
		EntityHandle Parent = entt::null;
		std::vector<EntityHandle> Children;
	};

}
