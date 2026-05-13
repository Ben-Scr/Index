#pragma once

namespace Index {
	struct IdTag {};
	struct StaticTag {};
	// WARNING: The cascade hooks (OnDisabledTagConstruct/Destruct in Scene.cpp)
	// recursively emplace/remove DisabledTag (and InheritedDisabledTag) on
	// child entities of the toggled entity. If you iterate a view<DisabledTag>
	// (or view<InheritedDisabledTag>) and toggle disable on a parent during
	// iteration, the cascade may invalidate your iterator — entt registry
	// mutation during iteration of the same pool is undefined. Snapshot the
	// view into a vector first if you need to mutate.
	struct DisabledTag {};
	struct InheritedDisabledTag {};
	struct DeadlyTag{};
}
